// SPDX-License-Identifier: Apache-2.0
// roam-331: the local-storage commit-ordering barrier for browser tests.

#ifndef ROAMUX_TEST_SUPPORT_STORAGE_FLUSH_BARRIER_H_
#define ROAMUX_TEST_SUPPORT_STORAGE_FLUSH_BARRIER_H_

namespace content {
class StoragePartition;
}  // namespace content

namespace roamux::test {

// Orders ALREADY-QUEUED local-storage commits against the caller, so a test can
// copy a profile's `Local Storage` directory and see the writes it made.
//
// LocalStorageControl::Flush() has no reply (local_storage_control.mojom) and
// only *initiates* commits: LocalStorageImpl::Flush calls
// StorageAreaImpl::ScheduleImmediateCommit, which reaches
// database_->InitiateCommit() and returns. This blocks on a replying GetUsage()
// on the same pipe; its metadata read and that commit both post to the same
// base::SequenceBound<DomStorageDatabase>, so FIFO means the commit has
// completed once GetUsage replies.
//
// NOT an acknowledgement that a mutation has REACHED the service. A renderer's
// localStorage.setItem sends an asynchronous StorageArea::Put, and ExecJs
// returning proves nothing about it — flushing before it arrives flushes
// nothing. Acknowledge the mutation first (see the GetAll-with-observer wait in
// roamux_edge_import_driver_browsertest.cc), then call this.
void FlushLocalStorageAndWait(content::StoragePartition* partition);

}  // namespace roamux::test

#endif  // ROAMUX_TEST_SUPPORT_STORAGE_FLUSH_BARRIER_H_
