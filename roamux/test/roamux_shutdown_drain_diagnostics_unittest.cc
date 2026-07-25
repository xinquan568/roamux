// SPDX-License-Identifier: Apache-2.0
//
// roam-201: coverage for the shutdown-drain diagnostics added by
// patches/0056-shutdown-drain-test-diagnostics.patch. The roamux_browsertests
// teardown stall fires TaskEnvironment's CompleteShutdown watchdog with
// "ThreadPool currently running tasks: none." — the blocker is a
// registered-but-not-running BLOCK_SHUTDOWN task source that the stock dump
// cannot name. The patch makes the watchdog self-attributing; these tests pin
// that contract against future Chromium rebases:
//   T1 inertness: a default-configured TaskTracker retains nothing.
//   T2 sequence identity: the real posting path surfaces posted_from on demand.
//   T3 counted registrations: re-registered sources are counted, not clobbered.
//   T4 bounded output: deterministic 32-entry cap with an omitted count.
//   T5 snapshot lifetime: describe races source unregistration safely.
//   T6 job identity: JobTaskSource renders its construction location.
//   T7 message composition: both diagnostic sections combine verbatim.
//   T8 real branch: the actual timeout branch emits both sections.
//   T9 empty-queue fallback: a popped-but-unrun task never fabricates identity.

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/raw_ref.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/execution_fence.h"
#include "base/task/post_job.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/task/thread_pool/job_task_source.h"
#include "base/task/thread_pool/sequence.h"
#include "base/task/thread_pool/task.h"
#include "base/task/thread_pool/task_tracker.h"
#include "base/task/thread_pool/thread_pool_impl.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/test/task_environment.h"
#include "base/threading/platform_thread.h"
#include "base/threading/simple_thread.h"
#include "base/time/time.h"
#include "testing/gtest/include/gtest/gtest-spi.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace roamux {
namespace {

using ::base::internal::TaskTracker;

// The file token every identity assertion looks for: gtest's FROM_HERE strings
// carry the source file name.
constexpr char kThisFile[] = "roamux_shutdown_drain_diagnostics_unittest.cc";

scoped_refptr<base::internal::Sequence> MakeBlockShutdownSequence() {
  return base::MakeRefCounted<base::internal::Sequence>(
      base::TaskTraits{base::MayBlock(),
                       base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      /*task_runner=*/nullptr,
      base::internal::TaskSourceExecutionMode::kSequenced,
      base::ThreadType::kDefault);
}

base::internal::Task MakeTask(const base::Location &from_here) {
  return base::internal::Task(from_here, base::DoNothing(),
                              base::TimeTicks::Now(), base::TimeDelta());
}

// Pushes `task` into `sequence` under a transaction, mirroring the immediate-
// task posting path far enough for TaskTracker registration semantics.
void PushTask(base::internal::Sequence *sequence, base::internal::Task task) {
  auto transaction = sequence->BeginTransaction();
  transaction.WillPushImmediateTask();
  transaction.PushImmediateTask(std::move(task));
}

// T1 — inertness: production configuration retains and reports nothing, even
// with BLOCK_SHUTDOWN work registered.
TEST(RoamuxShutdownDrainDiagnosticsTest, DefaultTrackerRetainsNothing) {
  TaskTracker tracker;
  EXPECT_EQ(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());

  auto sequence = MakeBlockShutdownSequence();
  PushTask(sequence.get(), MakeTask(FROM_HERE));
  auto registered = tracker.RegisterTaskSource(sequence);
  ASSERT_TRUE(registered);
  EXPECT_EQ(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());
}

// T2 — the real posting path: a task posted through an (unstarted)
// ThreadPoolImpl surfaces this file's posted_from on demand, and the entry
// clears once the pool drains it.
TEST(RoamuxShutdownDrainDiagnosticsTest, SequenceIdentityViaRealPostingPath) {
  auto owned_tracker = std::make_unique<TaskTracker>();
  owned_tracker->set_retain_block_shutdown_identity_for_testing(true);
  TaskTracker *tracker = owned_tracker.get();
  auto pool = std::make_unique<base::internal::ThreadPoolImpl>(
      "RoamuxShutdownDrainTest", std::move(owned_tracker));

  auto task_runner = pool->CreateSequencedTaskRunner(
      {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
  task_runner->PostTask(FROM_HERE, base::DoNothing());

  const std::string queued =
      tracker->DescribeIncompleteBlockShutdownTaskSourcesForTesting();
  EXPECT_NE(queued.find(kThisFile), std::string::npos) << queued;

  pool->Start(
      base::ThreadPoolInstance::InitParams(/*max_num_foreground_threads=*/2),
      /*worker_thread_observer=*/nullptr);
  pool->FlushForTesting();
  EXPECT_EQ(tracker->DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());
  pool->JoinForTesting();
}

// T3 — counted registrations: a source registered twice stays listed after one
// unregistration and clears only at zero.
TEST(RoamuxShutdownDrainDiagnosticsTest, RegistrationsAreCounted) {
  TaskTracker tracker;
  tracker.set_retain_block_shutdown_identity_for_testing(true);

  auto sequence = MakeBlockShutdownSequence();
  PushTask(sequence.get(), MakeTask(FROM_HERE));
  auto first = tracker.RegisterTaskSource(sequence);
  auto second = tracker.RegisterTaskSource(sequence);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  first.Unregister();
  EXPECT_NE(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting().find(
                kThisFile),
            std::string::npos);

  second.Unregister();
  EXPECT_EQ(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());
}

// T4 — bounded, deterministic output: more than 32 live sources render as
// exactly 32 entries plus an omitted count.
TEST(RoamuxShutdownDrainDiagnosticsTest, OutputIsBoundedAndDeterministic) {
  TaskTracker tracker;
  tracker.set_retain_block_shutdown_identity_for_testing(true);

  std::vector<base::internal::RegisteredTaskSource> registered;
  std::vector<scoped_refptr<base::internal::Sequence>> sequences;
  constexpr int kSources = 40;
  for (int i = 0; i < kSources; ++i) {
    auto sequence = MakeBlockShutdownSequence();
    PushTask(sequence.get(), MakeTask(FROM_HERE));
    registered.push_back(tracker.RegisterTaskSource(sequence));
    sequences.push_back(std::move(sequence));
  }

  const std::string dump =
      tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting();
  const std::string again =
      tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting();
  EXPECT_EQ(dump, again);
  size_t entries = 0;
  for (size_t pos = dump.find(kThisFile); pos != std::string::npos;
       pos = dump.find(kThisFile, pos + 1)) {
    ++entries;
  }
  EXPECT_EQ(entries, 32u);
  EXPECT_NE(dump.find("8 more"), std::string::npos) << dump;

  for (auto &source : registered) {
    source.Unregister();
  }
}

// T5 — snapshot lifetime: describing while another thread unregisters sources
// must not touch freed memory (refcounted snapshot).
TEST(RoamuxShutdownDrainDiagnosticsTest, DescribeRacesUnregistrationSafely) {
  TaskTracker tracker;
  tracker.set_retain_block_shutdown_identity_for_testing(true);

  std::vector<base::internal::RegisteredTaskSource> registered;
  std::vector<scoped_refptr<base::internal::Sequence>> sequences;
  for (int i = 0; i < 16; ++i) {
    auto sequence = MakeBlockShutdownSequence();
    PushTask(sequence.get(), MakeTask(FROM_HERE));
    registered.push_back(tracker.RegisterTaskSource(sequence));
    sequences.push_back(std::move(sequence));
  }

  class Unregisterer : public base::DelegateSimpleThread::Delegate {
  public:
    explicit Unregisterer(
        std::vector<base::internal::RegisteredTaskSource> &sources)
        : sources_(sources) {}
    void Run() override {
      for (auto &source : *sources_) {
        source.Unregister();
      }
    }

  private:
    const raw_ref<std::vector<base::internal::RegisteredTaskSource>> sources_;
  };

  Unregisterer unregisterer(registered);
  base::DelegateSimpleThread thread(&unregisterer, "RoamuxUnregisterer");
  thread.Start();
  for (int i = 0; i < 100; ++i) {
    tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting();
  }
  thread.Join();
  EXPECT_EQ(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());
}

// T6 — job identity: a registered BLOCK_SHUTDOWN JobTaskSource renders its
// construction from_here and clears on unregistration.
TEST(RoamuxShutdownDrainDiagnosticsTest, JobSourceRendersConstructionLocation) {
  // JobTaskSource requires a real PooledTaskRunnerDelegate; an unstarted
  // ThreadPoolImpl (with its own default tracker) serves as one.
  auto pool = std::make_unique<base::internal::ThreadPoolImpl>(
      "RoamuxShutdownDrainJobTest");
  TaskTracker tracker;
  tracker.set_retain_block_shutdown_identity_for_testing(true);

  auto job = base::MakeRefCounted<base::internal::JobTaskSource>(
      FROM_HERE, base::TaskTraits{base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::ThreadType::kDefault,
      base::BindRepeating([](base::JobDelegate *) {}),
      base::BindRepeating([](size_t) -> size_t { return 0; }), pool.get());
  auto registered = tracker.RegisterTaskSource(job);
  ASSERT_TRUE(registered);
  EXPECT_NE(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting().find(
                kThisFile),
            std::string::npos);
  registered.Unregister();
  EXPECT_EQ(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());

  // ThreadPoolImpl's destructor DCHECKs that JoinForTesting ran.
  pool->Start(base::ThreadPoolInstance::InitParams(1),
              /*worker_thread_observer=*/nullptr);
  pool->JoinForTesting();
}

// T7 — message composition: both sections combine verbatim, empty and
// populated.
TEST(RoamuxShutdownDrainDiagnosticsTest, TimeoutMessageComposesBothSections) {
  const std::string composed =
      base::test::ComposeCompleteShutdownTimeoutMessage(
          "ThreadPool currently running tasks: none.",
          "Registered incomplete BLOCK_SHUTDOWN task sources:\n  sample entry");
  EXPECT_NE(composed.find("running tasks: none."), std::string::npos);
  EXPECT_NE(composed.find("sample entry"), std::string::npos);

  const std::string empty_sources =
      base::test::ComposeCompleteShutdownTimeoutMessage(
          "ThreadPool currently running tasks: none.", std::string());
  EXPECT_NE(empty_sources.find("running tasks: none."), std::string::npos);
}

// T8 — the real timeout branch: with a genuinely queued BLOCK_SHUTDOWN task
// behind a true scheduling fence, invoking the actual BeginCompleteShutdown
// override (50 ms timeout, termination suppressed) emits BOTH sections into
// the captured nonfatal failure.
TEST(RoamuxShutdownDrainDiagnosticsTest, RealTimeoutBranchEmitsBothSections) {
  base::test::TaskEnvironment task_environment;
  task_environment.RunUntilIdle();

  base::test::TaskEnvironment::SetCompleteShutdownTimeoutForTesting(
      base::Milliseconds(50));
  base::test::TaskEnvironment::SetTerminateOnCompleteShutdownTimeoutForTesting(
      false);

  std::string captured;
  {
    base::ScopedThreadPoolExecutionFence fence;
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::DoNothing());

    base::WaitableEvent never_signaled;
    EXPECT_NONFATAL_FAILURE(
        {
          captured =
              task_environment.RunCompleteShutdownTimeoutBranchForTesting(
                  never_signaled);
        },
        "CompleteShutdown took more than");
    EXPECT_NE(captured.find("running tasks: none."), std::string::npos)
        << captured;
    EXPECT_NE(captured.find(kThisFile), std::string::npos) << captured;
  }
  task_environment.RunUntilIdle();

  base::test::TaskEnvironment::SetCompleteShutdownTimeoutForTesting(
      base::TimeDelta());
  base::test::TaskEnvironment::SetTerminateOnCompleteShutdownTimeoutForTesting(
      true);
}

// T9 — empty-queue fallback: a registered BLOCK_SHUTDOWN sequence with no
// queued task renders the explicit fallback, never fabricated identity.
TEST(RoamuxShutdownDrainDiagnosticsTest, EmptySequenceRendersFallback) {
  TaskTracker tracker;
  tracker.set_retain_block_shutdown_identity_for_testing(true);

  auto sequence = MakeBlockShutdownSequence();
  PushTask(sequence.get(), MakeTask(FROM_HERE));
  auto registered = tracker.RegisterTaskSource(sequence);
  ASSERT_TRUE(registered);

  // Pop the task the way a worker would, leaving the sequence registered but
  // empty.
  ASSERT_EQ(registered.WillRunTask(),
            base::internal::TaskSource::RunStatus::kAllowedSaturated);
  base::internal::Task taken = registered.TakeTask();

  const std::string dump =
      tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting();
  EXPECT_NE(dump.find("no queued task"), std::string::npos) << dump;
  EXPECT_EQ(dump.find(kThisFile), std::string::npos) << dump;
  taken.task.Reset();
  registered.DidProcessTask();
}

} // namespace
} // namespace roamux
