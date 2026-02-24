#include "recovery/group_commit.h"
#include "common/exceptions.h"
#include "common/utils.h"
#include "leanstore/config.h"
#include "leanstore/env.h"
#include "leanstore/leanstore.h"
#include "leanstore/statistics.h"
#include "recovery/log_manager.h"
#include "transaction/transaction_manager.h"

#include "share_headers/time.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <tuple>

using leanstore::transaction::CommitProtocol;

namespace leanstore::recovery {

GroupCommitExecutor::GroupCommitExecutor(buffer::BufferManager *buffer, LogManager *log_manager, u32 start_wid,
                                         u32 end_wid, std::atomic<bool> &keep_running)
    : buffer_(buffer),
      keep_running_(&keep_running),
      log_manager_(log_manager),
      start_logger_id_(start_wid),
      end_logger_id_(end_wid),
      ready_to_commit_cut_(FLAGS_worker_count),
      ready_to_commit_rfa_cut_(FLAGS_worker_count),
      worker_states_(FLAGS_worker_count) {
  int ret = io_uring_queue_init(GROUP_COMMIT_QD, &ring_, 0);
  if (ret != 0) { throw std::runtime_error("GroupCommit: io_uring_queue_init error"); }
}

/**
 * @brief Reset GroupCommitExecutor's state every round
 */
void GroupCommitExecutor::InitializeRound() {
  lp_req_.clear();
  already_prep_.clear();
  submitted_io_cnt_       = 0;
  completed_txn_          = 0;
  min_all_workers_gsn_    = std::numeric_limits<timestamp_t>::max();
  max_all_workers_gsn_    = 0;
  min_hardened_commit_ts_ = std::numeric_limits<timestamp_t>::max();
}

void GroupCommitExecutor::CompleteRound() {
  // Update RFA env
  UpdateMax(LogManager::global_min_gsn_flushed, min_all_workers_gsn_);
  UpdateMax(LogManager::global_sync_to_this_gsn, max_all_workers_gsn_);

  // Update statistics
  if (start_profiling) {
    statistics::txn_processed[LeanStore::worker_thread_id] += completed_txn_;
    statistics::commit_rounds[LeanStore::worker_thread_id]++;
    statistics::txn_per_round[LeanStore::worker_thread_id].emplace_back(completed_txn_);
    statistics::recovery::gct_phase_1_ns[LeanStore::worker_thread_id] +=
      tsctime::TscDifferenceNs(phase_1_begin_, phase_2_begin_);
    statistics::recovery::gct_phase_2_ns[LeanStore::worker_thread_id] +=
      tsctime::TscDifferenceNs(phase_2_begin_, phase_3_begin_);
    statistics::recovery::gct_phase_3_ns[LeanStore::worker_thread_id] +=
      tsctime::TscDifferenceNs(phase_3_begin_, phase_3_end_);
  }
}

/**
 * @brief Phase description:
 *
 * - Phase one: Write the current log buffer to the storage
 * - Phase two: Flush (fdatasync) all logs to disk and acknowledge all log entries are flushed to the storage
 * - Phase three: Write all BLOBs of transactions to the storage
 * - Phase four: Flush those BLOBs and mark them ok for evicted
 * - Phase five: Notify which transactions are committed according to RFA
 */
void GroupCommitExecutor::ExecuteOneRound() {
  if (keep_running_->load()) {
    InitializeRound();
    phase_1_begin_ = tsctime::ReadTSC();
    PhaseOne();
    phase_2_begin_ = tsctime::ReadTSC();
    if (FLAGS_blob_enable) { PhaseTwo(); }
    phase_3_begin_ = tsctime::ReadTSC();
    PhaseThree();
    phase_3_end_ = tsctime::ReadTSC();
    CompleteRound();
  }
}

void GroupCommitExecutor::StartExecution() {
  while (keep_running_->load()) {
    try {
      ExecuteOneRound();
    } catch (...) {
      /**
       * @brief The Group-commit thread is detached,
       *  which means LeanStore may be stopped & deallocated while the Group commit is still running.
       * Therefore, the group commit may cause SEGFAULT while LeanStore is deallocating,
       *  and this try-catch block is to silent that run-time problem
       */
      return;
    }
  }
}

// -------------------------------------------------------------------------------------

/**
 * @brief Collect committed state of all workers.
 * If group commit/flush pipelining, write logs using async I/O
 */
void GroupCommitExecutor::PhaseOne() {
  for (auto w_i = start_logger_id_; w_i < end_logger_id_; w_i++) { CollectPrecommittedQueue(w_i); }
  for (auto w_i = 0UL; w_i < FLAGS_worker_count; w_i++) { CollectConsistentState(w_i); }
  /* Persist all WAL to SSD using async IO */
  if ((FLAGS_txn_commit_variant != CommitProtocol::WORKERS_WRITE_LOG) &&
      (FLAGS_txn_commit_variant != CommitProtocol::AUTONOMOUS_COMMIT)) {
    for (auto w_i = start_logger_id_; w_i < end_logger_id_; w_i++) {
      log_manager_->logger_[w_i].log_buffer.WriteLogBuffer(worker_states_[w_i].last_wal_cursor, BLK_BLOCK_SIZE,
                                                           [&](u8 *buffer, u64 len) {
                                                             auto offset = log_manager_->w_offset_.fetch_sub(len);
                                                             PrepareWrite(buffer, len, offset - len);
                                                           });
    }
    if (submitted_io_cnt_ > 0) {
      UringSubmit(&ring_, submitted_io_cnt_);
      submitted_io_cnt_ = 0;
    }
  }
  /* Fsync() if required */
  if (FLAGS_wal_fsync) { Fsync(); }
  /* Acknowledge all log entries of other workers are flushed to the storage */
  if ((FLAGS_txn_commit_variant != CommitProtocol::WORKERS_WRITE_LOG) &&
      (FLAGS_txn_commit_variant != CommitProtocol::AUTONOMOUS_COMMIT)) {
    for (size_t w_i = start_logger_id_; w_i < end_logger_id_; w_i++) {
      log_manager_->logger_[w_i].log_buffer.write_cursor.store(worker_states_[w_i].last_wal_cursor,
                                                               std::memory_order_release);
    }
  }
}

/**
 * @brief LargePage phase - writing and persisting all extents of transactions
 */
void GroupCommitExecutor::PhaseTwo() {
  Ensure(FLAGS_blob_enable);
  for (size_t w_i = start_logger_id_; w_i < end_logger_id_; w_i++) {
    auto &logger = log_manager_->logger_[w_i];
    /**
     * @brief Persist all BLOBs for all transactions
     */
    if (FLAGS_dynamic_resizing) {
      logger.precommitted_queue.LoopElements_DR(ready_to_commit_cut_[w_i],
                                                logger.precommitted_queue.CurrentWriteBlock_DR(), [&](auto &txn) {
                                                  PrepareLargePageWrite(txn);
                                                  return true;
                                                });
      logger.precommitted_queue_rfa.LoopElements_DR(
        ready_to_commit_rfa_cut_[w_i], logger.precommitted_queue_rfa.CurrentWriteBlock_DR(), [&](auto &txn) {
          PrepareLargePageWrite(txn);
          return true;
        });
    } else {
      logger.precommitted_queue.LoopElements(ready_to_commit_cut_[w_i], [&](auto &txn) {
        PrepareLargePageWrite(txn);
        return true;
      });
      logger.precommitted_queue_rfa.LoopElements(ready_to_commit_rfa_cut_[w_i], [&](auto &txn) {
        PrepareLargePageWrite(txn);
        return true;
      });
    }
  }

  if (!lp_req_.empty()) {
    Ensure(submitted_io_cnt_ > 0);
    UringSubmit(&ring_, submitted_io_cnt_);
    submitted_io_cnt_ = 0;
    if (FLAGS_wal_fsync) { Fsync(); }

    for (auto [start_pid, pg_cnt, extent_pid] : lp_req_) {
      if (!completed_lp_.contains(start_pid)) {
        completed_lp_.add(start_pid);
        buffer_->EvictExtent(extent_pid);
      }
    }
  }
}

void GroupCommitExecutor::PhaseThree() {
  for (size_t w_i = start_logger_id_; w_i < end_logger_id_; w_i++) {
    auto &logger = log_manager_->logger_[w_i];

    /* Complete normal-transaction queue */
    auto committed_txn = 0UL;
    if (FLAGS_dynamic_resizing) {
      if (FLAGS_batch_looping) {
        auto [loop_bytes, read_txn, read_txn_b, new_r_block, committed_txn] = logger.precommitted_queue.Batch_Loop(
          ready_to_commit_cut_[w_i], logger.precommitted_queue.CurrentWriteBlock_DR(),
          [&](auto &txn) { return SatisfyCommitConditions(w_i, txn); });

        if (!(loop_bytes == 0 && read_txn == 0 && new_r_block == logger.precommitted_queue.CurrentReadBlock_DR() &&
              committed_txn == 0)) {
          logger.precommitted_queue.Erase_DR(loop_bytes, read_txn, read_txn_b, new_r_block);

          completed_txn_ += committed_txn;

          auto commit_stats = tsctime::ReadTSC();

          statistics::txn_stats_committed[w_i].emplace_back(committed_txn, commit_stats, phase_2_begin_);
        }

        std::tie(loop_bytes, read_txn, read_txn_b, new_r_block, committed_txn) =
          logger.precommitted_queue_rfa.Batch_Loop(
            ready_to_commit_rfa_cut_[w_i], logger.precommitted_queue_rfa.CurrentWriteBlock_DR(),
            [&](auto &txn) { return txn.commit_ts <= worker_states_[w_i].precommitted_tx_commit_ts; });
        if (!(loop_bytes == 0 && read_txn == 0 && new_r_block == logger.precommitted_queue_rfa.CurrentReadBlock_DR() &&
              committed_txn == 0)) {
          logger.precommitted_queue_rfa.Erase_DR(loop_bytes, read_txn, read_txn_b, new_r_block);
          completed_txn_ += committed_txn;
          auto commit_stats = tsctime::ReadTSC();
          statistics::txn_stats_rfa_committed[w_i].emplace_back(committed_txn, commit_stats, phase_2_begin_);
        }
      } else {
        auto [loop_bytes, read_txn, read_txn_b, new_r_block] = logger.precommitted_queue.LoopElements_DR(
          ready_to_commit_cut_[w_i], logger.precommitted_queue.CurrentWriteBlock_DR(), [&](auto &txn) {
            if (SatisfyCommitConditions(w_i, txn)) {
              if (txn.state != transaction::Transaction::State::BARRIER) {
                committed_txn++;

                CompleteTransaction(txn);
              }
              return true;
            }
            return false;
          });
        if (!(loop_bytes == 0 && read_txn == 0 && new_r_block == logger.precommitted_queue.CurrentReadBlock_DR())) {
          logger.precommitted_queue.Erase_DR(loop_bytes, read_txn, read_txn_b, new_r_block);
          completed_txn_ += committed_txn;
        }

        /* Process RFA-transaction queue */
        committed_txn                                           = 0;
        std::tie(loop_bytes, read_txn, read_txn_b, new_r_block) = logger.precommitted_queue_rfa.LoopElements_DR(
          ready_to_commit_rfa_cut_[w_i], logger.precommitted_queue_rfa.CurrentWriteBlock_DR(), [&](auto &txn) {
            if (txn.commit_ts <= worker_states_[w_i].precommitted_tx_commit_ts) [[likely]] {
              if (txn.state != transaction::Transaction::State::BARRIER) {
                committed_txn++;
                CompleteTransaction(txn);
              }
              return true;
            }
            return false;
          });
        if (!(loop_bytes == 0 && read_txn == 0 && new_r_block == logger.precommitted_queue_rfa.CurrentReadBlock_DR())) {
          logger.precommitted_queue_rfa.Erase_DR(loop_bytes, read_txn, read_txn_b, new_r_block);
          completed_txn_ += committed_txn;
        }
      }
    } else {
      auto [loop_bytes, no_txn] = logger.precommitted_queue.LoopElements(ready_to_commit_cut_[w_i], [&](auto &txn) {
        if (SatisfyCommitConditions(w_i, txn)) {
          if (txn.state != transaction::Transaction::State::BARRIER) {
            committed_txn++;
            CompleteTransaction(txn);
          }
          return true;
        }
        return false;
      });
      if (loop_bytes > 0) {
        logger.precommitted_queue.Erase(loop_bytes, no_txn);
        completed_txn_ += committed_txn;
      }

      /* Process RFA-transaction queue */
      committed_txn = 0;
      std::tie(loop_bytes, no_txn) =
        logger.precommitted_queue_rfa.LoopElements(ready_to_commit_rfa_cut_[w_i], [&](auto &txn) {
          if (txn.commit_ts <= worker_states_[w_i].precommitted_tx_commit_ts) [[likely]] {
            if (txn.state != transaction::Transaction::State::BARRIER) {
              committed_txn++;
              CompleteTransaction(txn);
            }
            return true;
          }
          return false;
        });
      if (loop_bytes > 0) {
        logger.precommitted_queue_rfa.Erase(loop_bytes, no_txn);
        completed_txn_ += committed_txn;
      }
    }
  }
}

// -------------------------------------------------------------------------------------

void GroupCommitExecutor::Fsync() { fdatasync(log_manager_->wal_fd_); }

void GroupCommitExecutor::CollectConsistentState(wid_t w_i) {
  if (FLAGS_txn_commit_variant != CommitProtocol::AUTONOMOUS_COMMIT) {
    worker_states_[w_i].Clone(log_manager_->w_state_[w_i]);
  } else {
    worker_states_[w_i].Clone(log_manager_->commit_state_[w_i]);
  }
  min_all_workers_gsn_ = std::min<timestamp_t>(min_all_workers_gsn_, worker_states_[w_i].last_gsn);
  max_all_workers_gsn_ = std::max<timestamp_t>(max_all_workers_gsn_, worker_states_[w_i].last_gsn);
  min_hardened_commit_ts_ =
    std::min<timestamp_t>(min_hardened_commit_ts_, worker_states_[w_i].precommitted_tx_commit_ts);
}

void GroupCommitExecutor::CollectPrecommittedQueue(wid_t w_i) {
  auto &logger = log_manager_->logger_[w_i];
  if (FLAGS_dynamic_resizing) {
    ready_to_commit_cut_[w_i]     = logger.precommitted_queue.CurrentTail_DR();
    ready_to_commit_rfa_cut_[w_i] = logger.precommitted_queue_rfa.CurrentTail_DR();
  } else {
    ready_to_commit_cut_[w_i]     = logger.precommitted_queue.CurrentTail();
    ready_to_commit_rfa_cut_[w_i] = logger.precommitted_queue_rfa.CurrentTail();
  }
}

void GroupCommitExecutor::PrepareWrite(u8 *src, size_t size, size_t offset) {
  Ensure(u64(src) % BLK_BLOCK_SIZE == 0);
  Ensure(size % BLK_BLOCK_SIZE == 0);
  // Ensure(offset % BLK_BLOCK_SIZE == 0);
  auto sqe = io_uring_get_sqe(&ring_);
  if (sqe == nullptr) {
    UringSubmit(&ring_, submitted_io_cnt_);
    sqe = io_uring_get_sqe(&ring_);
    Ensure(sqe != nullptr);
    submitted_io_cnt_ = 0;
  }
  submitted_io_cnt_++;
  io_uring_prep_write(sqe, log_manager_->wal_fd_, src, size, offset);
}

void GroupCommitExecutor::PrepareLargePageWrite(const transaction::SerializableTransaction &txn) {
  Ensure(txn.ToFlushedLargePages().size() == txn.ToEvictedExtents().size());

  for (auto &[pid, pg_cnt] : txn.ToFlushedLargePages()) {
    if (!already_prep_.contains(pid)) {
      already_prep_.add(pid);
      if (FLAGS_blob_normal_buffer_pool) {
        for (auto id = pid; id < pid + pg_cnt; id++) {
          PrepareWrite(reinterpret_cast<u8 *>(buffer_->ToPtr(id)), PAGE_SIZE, id * PAGE_SIZE);
        }
      } else {
        PrepareWrite(reinterpret_cast<u8 *>(buffer_->ToPtr(pid)), pg_cnt * PAGE_SIZE, pid * PAGE_SIZE);
      }
    }
  }

  /* Update large-page requirements for txn commit */
  for (size_t idx = 0; idx < txn.ToFlushedLargePages().size(); idx++) {
    auto &[pid, pg_cnt] = txn.ToFlushedLargePages()[idx];
    auto &extent_pid    = txn.ToEvictedExtents()[idx];
    if (start_profiling) { statistics::blob::blob_logging_io += pg_cnt * PAGE_SIZE; }
    lp_req_.emplace_back(pid, pg_cnt, extent_pid);
  }
}

void GroupCommitExecutor::CompleteTransaction(transaction::SerializableTransaction &txn) {
  Ensure(txn.state == transaction::Transaction::State::READY_TO_COMMIT);
  if (FLAGS_blob_enable) {
    for (auto &lp : txn.ToFlushedLargePages()) { completed_lp_.remove(lp.start_pid); }
    buffer_->FreeStorageManager()->PublicFreeExtents(txn.ToFreeExtents());
  }
  transaction::TransactionManager::DurableCommit(txn, phase_2_begin_);
}

auto GroupCommitExecutor::SatisfyCommitConditions(wid_t w_i, const transaction::SerializableTransaction &txn) -> bool {
  if (FLAGS_wal_variant != LoggingVariant::VECTOR) {
    /**
     * @brief In GSN/RFA decentralized logging, there are two conditions for commit operations:
     * - The transaction didn't see any uncommitted content (from a possible dependency)
     * - Dependent transactions are hardened, i.e., their logs are persistent, required for Early Lock Release
     *    + See paper: Scalable and Robust Snapshot Isolation for High-Performance Storage Engines"
     */
    return txn.max_observed_gsn <= min_all_workers_gsn_ && txn.commit_ts <= min_hardened_commit_ts_;
  }
  /**
   * @brief With GSN Vector variant, commit conditions is:
   * - Committed locally (using commit_ts is equivalent to local GSN in this case)
   * - Dependencies' GSN are lower than the durable GSN of those workers
   */
  if (txn.commit_ts > worker_states_[w_i].precommitted_tx_commit_ts) { return false; }
  for (auto idx = 0UL; idx < txn.vector_size; idx++) {
    auto wid = txn.Dependencies()[idx];
    if (worker_states_[wid].last_gsn < txn.DepGSN()[idx]) { return false; }
  }
  return true;
}

}  // namespace leanstore::recovery
