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
#include <string>
#include <utility>
#include <vector>

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
  EXPECT_NE(queued.find("priority=USER_VISIBLE"), std::string::npos) << queued;

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

// T5 — snapshot lifetime, deterministic: the describe-snapshot seam pauses a
// describe (on a helper thread) right after its refcounted snapshot is taken;
// the main thread then unregisters every source and drops the only other
// refs; rendering resumes against sources whose sole owner is the snapshot.
// Without snapshot refs this is a deterministic use-after-free.
TEST(RoamuxShutdownDrainDiagnosticsTest, DescribeRacesUnregistrationSafely) {
  TaskTracker tracker;
  tracker.set_retain_block_shutdown_identity_for_testing(true);

  std::vector<base::internal::RegisteredTaskSource> registered;
  for (int i = 0; i < 16; ++i) {
    auto sequence = MakeBlockShutdownSequence();
    PushTask(sequence.get(), MakeTask(FROM_HERE));
    registered.push_back(tracker.RegisterTaskSource(std::move(sequence)));
  }

  base::WaitableEvent snapshot_taken;
  base::WaitableEvent resume_render;
  tracker.SetDescribeSnapshotTakenClosureForTesting(base::BindRepeating(
      [](base::WaitableEvent *taken, base::WaitableEvent *resume) {
        taken->Signal();
        resume->Wait();
      },
      &snapshot_taken, &resume_render));

  class Describer : public base::DelegateSimpleThread::Delegate {
  public:
    explicit Describer(TaskTracker &tracker) : tracker_(tracker) {}
    void Run() override {
      dump_ = tracker_->DescribeIncompleteBlockShutdownTaskSourcesForTesting();
    }
    const std::string &dump() const { return dump_; }

  private:
    const raw_ref<TaskTracker> tracker_;
    std::string dump_;
  };

  Describer describer(tracker);
  base::DelegateSimpleThread thread(&describer, "RoamuxDescriber");
  thread.Start();

  snapshot_taken.Wait();
  // The snapshot exists; destroy every other owner.
  for (auto &source : registered) {
    source.Unregister();
  }
  registered.clear();
  resume_render.Signal();
  thread.Join();

  // Rendering completed against snapshot-owned sources: full dump, no UAF.
  EXPECT_NE(describer.dump().find(kThisFile), std::string::npos)
      << describer.dump();
  tracker.SetDescribeSnapshotTakenClosureForTesting(base::RepeatingClosure());
  EXPECT_EQ(tracker.DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());
}

// T6 — job identity through the real pool job path: a BLOCK_SHUTDOWN
// JobTaskSource enqueued into an unstarted ThreadPoolImpl (whose tracker
// retains identity) renders its construction from_here while queued, and the
// entry clears once the pool drains the job.
TEST(RoamuxShutdownDrainDiagnosticsTest, JobSourceRendersConstructionLocation) {
  auto owned_tracker = std::make_unique<TaskTracker>();
  owned_tracker->set_retain_block_shutdown_identity_for_testing(true);
  TaskTracker *tracker = owned_tracker.get();
  auto pool = std::make_unique<base::internal::ThreadPoolImpl>(
      "RoamuxShutdownDrainJobTest", std::move(owned_tracker));

  std::atomic_size_t remaining{1};
  auto job = base::MakeRefCounted<base::internal::JobTaskSource>(
      FROM_HERE, base::TaskTraits{base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
      base::ThreadType::kDefault,
      base::BindRepeating([](std::atomic_size_t *remaining,
                             base::JobDelegate *) { remaining->store(0); },
                          &remaining),
      base::BindRepeating(
          [](std::atomic_size_t *remaining, size_t /*worker_count*/) -> size_t {
            return remaining->load();
          },
          &remaining),
      pool.get());
  ASSERT_TRUE(pool->EnqueueJobTaskSource(job));

  const std::string queued =
      tracker->DescribeIncompleteBlockShutdownTaskSourcesForTesting();
  EXPECT_NE(queued.find(kThisFile), std::string::npos) << queued;
  EXPECT_NE(queued.find("job created from:"), std::string::npos) << queued;

  pool->Start(base::ThreadPoolInstance::InitParams(1),
              /*worker_thread_observer=*/nullptr);
  pool->FlushForTesting();
  EXPECT_EQ(tracker->DescribeIncompleteBlockShutdownTaskSourcesForTesting(),
            std::string());
  pool->JoinForTesting();
}

// T7 — message composition: all four operand combinations compose cleanly
// (no stray separators, sections verbatim).
TEST(RoamuxShutdownDrainDiagnosticsTest, TimeoutMessageComposesBothSections) {
  const std::string running = "ThreadPool currently running tasks: none.";
  const std::string sources =
      "Registered incomplete BLOCK_SHUTDOWN task sources:\n  sample entry";

  const std::string both =
      base::test::ComposeCompleteShutdownTimeoutMessage(running, sources);
  EXPECT_EQ(both, running + "\n" + sources);

  EXPECT_EQ(
      base::test::ComposeCompleteShutdownTimeoutMessage(running, std::string()),
      running);
  EXPECT_EQ(
      base::test::ComposeCompleteShutdownTimeoutMessage(std::string(), sources),
      sources);
  EXPECT_EQ(base::test::ComposeCompleteShutdownTimeoutMessage(std::string(),
                                                              std::string()),
            std::string());
}

// T8 — the real timeout branch: with a genuinely queued BLOCK_SHUTDOWN task
// behind a true scheduling fence, invoking the actual BeginCompleteShutdown
// override (50 ms timeout, termination suppressed) reports ONE nonfatal
// failure whose message contains BOTH diagnostic sections.
TEST(RoamuxShutdownDrainDiagnosticsTest, RealTimeoutBranchEmitsBothSections) {
  base::test::TaskEnvironment task_environment;
  task_environment.RunUntilIdle();

  base::test::TaskEnvironment::SetCompleteShutdownTimeoutForTesting(
      base::Milliseconds(50));
  base::test::TaskEnvironment::SetTerminateOnCompleteShutdownTimeoutForTesting(
      false);

  {
    base::ScopedThreadPoolExecutionFence fence;
    base::ThreadPool::PostTask(
        FROM_HERE,
        {base::MayBlock(), base::TaskShutdownBehavior::BLOCK_SHUTDOWN},
        base::DoNothing());

    base::WaitableEvent never_signaled;
    testing::TestPartResultArray failures;
    {
      testing::ScopedFakeTestPartResultReporter reporter(
          testing::ScopedFakeTestPartResultReporter::
              INTERCEPT_ONLY_CURRENT_THREAD,
          &failures);
      task_environment.RunCompleteShutdownTimeoutBranchForTesting(
          never_signaled);
    }
    ASSERT_EQ(failures.size(), 1);
    const std::string message = failures.GetTestPartResult(0).message();
    EXPECT_NE(message.find("CompleteShutdown took more than"),
              std::string::npos)
        << message;
    EXPECT_NE(message.find("running tasks: none."), std::string::npos)
        << message;
    EXPECT_NE(message.find("Registered incomplete BLOCK_SHUTDOWN"),
              std::string::npos)
        << message;
    EXPECT_NE(message.find(kThisFile), std::string::npos) << message;
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
