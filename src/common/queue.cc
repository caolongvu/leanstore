#include "common/queue.h"
#include "common/rand.h"
#include "common/utils.h"
#include "leanstore/leanstore.h"
#include "leanstore/statistics.h"
#include "share_headers/time.h"
#include "sync/hybrid_guard.h"
#include "transaction/transaction.h"

#include <sys/mman.h>
#include <atomic>
#include <iostream>

namespace leanstore {

// ----------------------------------------------------------------------------------------------

template <typename T>
LockFreeQueue<T>::LockFreeQueue() : buffer_capacity_(FLAGS_txn_queue_size_mb * MB) {
  assert(std::is_trivially_destructible_v<T>);
  if (FLAGS_dynamic_resizing) {
    Ensure((buffer_capacity_ & (buffer_capacity_ - 1)) == 0);
    QueueBlock *first_block = new QueueBlock(buffer_capacity_);
    QueueBlock *last_block  = first_block;

    for (u64 i = 1; i < 5; ++i) {
      QueueBlock *next_block = new QueueBlock(buffer_capacity_);
      last_block->next.store(next_block, std::memory_order_relaxed);
      last_block = next_block;
    }

    last_block->next.store(first_block, std::memory_order_relaxed);
    current_write_block_.store(first_block, std::memory_order_release);
    current_read_block_.store(first_block, std::memory_order_release);

    /*QueueBlock *first_block = new QueueBlock(buffer_capacity_);
    first_block->next.store(first_block, std::memory_order_release);
    current_write_block_.store(first_block, std::memory_order_release);
    current_read_block_.store(first_block, std::memory_order_release);*/

  } else {
    buffer_ = reinterpret_cast<u8 *>(AllocHuge(buffer_capacity_));
  }
}

template <typename T>
LockFreeQueue<T>::~LockFreeQueue() {
  if (FLAGS_dynamic_resizing) {
    QueueBlock *first_block = current_write_block_.load(std::memory_order_acquire);
    QueueBlock *current     = first_block;
    while (current->next.load(std::memory_order_acquire) != first_block) {
      auto next = current->next.load(std::memory_order_acquire);
      delete current;
      current = next;
    }

    delete current;

  } else {
    munmap(buffer_, buffer_capacity_);
  }
}

/**
 * @brief Push a serialized element to the queue from unserialized data with dynamic resizing
 */

template <typename T>
template <typename T2>
void LockFreeQueue<T>::Push_DR(const T2 &element) {
  u64 push_stat_start = tsctime::ReadTSC();

  u64 item_size = static_cast<uoffset_t>(element.SerializedSize());
  Ensure(item_size == 64);
  QueueBlock *w_block = current_write_block_.load(std::memory_order_relaxed);
  u64 w_tail          = w_block->tail.load(std::memory_order_relaxed);
  u64 r_head          = w_block->head.load(std::memory_order_acquire);

  /* Allocate new queueblock */
  if (__builtin_expect(((w_tail + item_size) & w_block->mask) == r_head, 0)) {
    std::printf("Next\n");
    QueueBlock *next = w_block->next.load(std::memory_order_relaxed);
    if (next->tail.load(std::memory_order_relaxed) != next->head.load(std::memory_order_acquire)) {
      std::printf("Jump\n");
      QueueBlock *new_block = new QueueBlock(w_block->buffer_capacity);
      new_block->next.store(next, std::memory_order_relaxed);
      w_block->next.store(new_block, std::memory_order_relaxed);
      next = new_block;
    }
    w_block = next;
    w_tail  = w_block->tail.load(std::memory_order_relaxed);
    current_write_block_.store(next, std::memory_order_relaxed);
  }

  /* Have enough memory -> producer write the element to the buffer */
  Ensure(w_tail % CPU_CACHELINE_SIZE == 0);
  auto obj = reinterpret_cast<T *>(&w_block->buffer[w_tail]);
  obj->Construct(element);
  w_block->tail.store((w_tail + item_size) & w_block->mask, std::memory_order_release);

  statistics::push_stats[LeanStore::worker_thread_id].emplace_back(
    tsctime::TscDifferenceNs(push_stat_start, tsctime::ReadTSC()));
}

template <typename T>
void LockFreeQueue<T>::Erase_DR(u64 no_bytes, QueueBlock *new_r_block) {
  u64 erase_stat_start = tsctime::ReadTSC();

  QueueBlock *r_block = current_read_block_.load(std::memory_order_relaxed);

  while (r_block != new_r_block) {
    r_block->head.store(r_block->tail.load(std::memory_order_relaxed), std::memory_order_release);
    r_block = r_block->next.load(std::memory_order_relaxed);
  }

  Ensure(r_block = new_r_block);
  u64 r_head = r_block->head.load(std::memory_order_relaxed);
  current_read_block_.store(r_block, std::memory_order_relaxed);

  r_block->head.store((r_head + no_bytes) & r_block->mask, std::memory_order_release);

  statistics::erase_stats[LeanStore::worker_thread_id].emplace_back(
    tsctime::TscDifferenceNs(erase_stat_start, tsctime::ReadTSC()));
}

template <typename T>
auto LockFreeQueue<T>::LoopElements_DR(u64 until_tail, QueueBlock *tail_block, const std::function<bool(T &)> &read_cb)
  -> std::tuple<u64, QueueBlock *> {
  u64 loop_stat_start = tsctime::ReadTSC();
  QueueBlock *r_block = current_read_block_.load(std::memory_order_relaxed);
  u64 r_head          = r_block->head.load(std::memory_order_relaxed);
  u64 no_bytes        = 0;

  while (!(r_block == tail_block && r_head == until_tail)) {
    const auto item = reinterpret_cast<T *>(&r_block->buffer[r_head]);

    if (!read_cb(*item)) { break; }
    r_head = (r_head + item->MemorySize()) & r_block->mask;
    no_bytes += item->MemorySize();
    if (r_block != tail_block) {
      u64 w_tail = r_block->tail.load(std::memory_order_acquire);
      if (r_head == w_tail) {
        no_bytes = 0;
        r_block  = r_block->next.load(std::memory_order_relaxed);
        r_head   = r_block->head.load(std::memory_order_relaxed);
      }
    }
  }

  statistics::loop_stats[LeanStore::worker_thread_id].emplace_back(
    tsctime::TscDifferenceNs(loop_stat_start, tsctime::ReadTSC()));

  return std::make_tuple(no_bytes, r_block);
}

template <typename T>
auto LockFreeQueue<T>::Batch_Loop(u64 until_tail, QueueBlock *tail_block, const std::function<bool(T &)> &read_cb)
  -> std::tuple<u64, QueueBlock *, u64> {
  u64 loop_stat_start = tsctime::ReadTSC();
  QueueBlock *r_block = current_read_block_.load(std::memory_order_relaxed);
  u64 r_head          = r_block->head.load(std::memory_order_relaxed);
  u64 w_tail          = until_tail;
  u64 old_head        = r_head;
  u64 free_bytes      = 0;
  u64 item_size       = 64;
  bool batching       = true;
  u64 no_bytes        = 0;
  u64 no_txn          = 0;

  if (r_block != tail_block) { w_tail = r_block->tail.load(std::memory_order_acquire); }

  while (!(r_block == tail_block && r_head == until_tail)) {
    if (r_block != tail_block && r_head == w_tail) {
      no_bytes = 0;
      r_block  = r_block->next.load(std::memory_order_relaxed);
      r_head   = r_block->head.load(std::memory_order_relaxed);
      if (r_block != tail_block) {
        w_tail = r_block->tail.load(std::memory_order_acquire);
      } else {
        w_tail = until_tail;
      }
      continue;
    }
    old_head = r_head;
    if (batching) {
      free_bytes = ContiguousFreeBytes(r_head, w_tail);
      if (free_bytes > (FLAGS_batch_looping_step_size * item_size)) {
        r_head = (r_head + (FLAGS_batch_looping_step_size * item_size)) & r_block->mask;
      } else {
        r_head = (r_head + free_bytes - item_size) & r_block->mask;
      }
    }

    const auto item = reinterpret_cast<T *>(&r_block->buffer[r_head]);
    if (!read_cb(*item)) {
      if (batching) {
        r_head   = old_head;
        batching = false;
        continue;
      } else {
        break;
      }
    }

    r_head = (r_head + item->MemorySize()) & r_block->mask;
    no_bytes += ContiguousFreeBytes(old_head, r_head);
    no_txn += ContiguousFreeBytes(old_head, r_head) / item_size;
  }

  statistics::loop_stats[LeanStore::worker_thread_id].emplace_back(
    tsctime::TscDifferenceNs(loop_stat_start, tsctime::ReadTSC()));

  return std::make_tuple(no_bytes, r_block, no_txn);
}

/**
 * @brief Erase multiple items at once
 * `no_bytes` starting from `head_.load()` should perfectly store `n_items` queued items
 * i.e., you should call SizeApprox() -> LoopElement() -> Erase():
 * - SizeApprox(): Return the number of queued items at the moment
 * - LoopElement(): Loop through all these elements and return total number of bytes these items consume
 * - Erase(): Remove `n_items` from SizeApprox() and `no_bytes` from LoopElement()
 */
template <typename T>
void LockFreeQueue<T>::Erase(u64 no_bytes, u64 no_txn) {
  u64 erase_stat_start = tsctime::ReadTSC();
  if (no_bytes <= 0) { return; }
  auto r_head = head_.load(std::memory_order_acquire);

  if (r_head + no_bytes < buffer_capacity_) {
    r_head += no_bytes;
  } else {
    r_head = no_bytes - (buffer_capacity_ - r_head);
  }
  head_.store(r_head, std::memory_order_release);
  no_txn_.fetch_add(-no_txn, std::memory_order_release);

  statistics::erase_stats[LeanStore::worker_thread_id].emplace_back(
    tsctime::TscDifferenceNs(erase_stat_start, tsctime::ReadTSC()));
}

/**
 * @brief Return the byte offset at which the loop stops
 *
 * Users have to provide a `read_cb` to process with each loop item and
 *  return true/false whether the users want to continue the loop or not.
 * Note that, if the `read_cb` return false, that evaluated object will be re-evaluated in next iteration
 * The queue may also stop looping if it reaches the last queued item, i.e., r_head == until_tail
 */
template <typename T>
auto LockFreeQueue<T>::LoopElements(u64 until_tail, const std::function<bool(T &)> &read_cb) -> std::tuple<u64, u64> {
  u64 loop_stat_start = tsctime::ReadTSC();
  auto r_head         = head_.load(std::memory_order_acquire);
  auto curr_no_txn    = no_txn_.load(std::memory_order_acquire);
  auto old_head       = r_head;
  auto no_txn         = 0;
  auto no_bytes       = 0;

  while (curr_no_txn - no_txn > 0) {
    /* Circular back to the beginning of the buffer if deadend meet */
    if (T::InvalidByteBuffer(&buffer_[r_head])) { r_head = 0; }

    /* Read the queued item */
    const auto item = reinterpret_cast<T *>(&buffer_[r_head]);
    if (!read_cb(*item)) { break; }
    r_head += item->MemorySize();
    no_txn++;
  }

  if (r_head > old_head) {
    no_bytes = r_head - old_head;
  } else {
    if (no_txn != 0) { no_bytes = r_head + buffer_capacity_ - old_head; }
  }

  /*if (statistics::total_committed_txn > 700000) {
    std::cout << "r_head = " << r_head << " old_head = " << old_head << " until_tail = " << until_tail << std::endl;
  }*/

  statistics::loop_stats[LeanStore::worker_thread_id].emplace_back(
    tsctime::TscDifferenceNs(loop_stat_start, tsctime::ReadTSC()));

  return std::make_tuple(no_bytes, no_txn);
}

// ----------------------------------------------------------------------------------------------

template <class T>
auto ConcurrentQueue<T>::LoopElement(u64 no_elements, const std::function<bool(T &)> &fn) -> u64 {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  auto idx = 0UL;
  for (; idx < no_elements; idx++) {
    if (!fn(internal_[idx])) { break; }
  }
  return idx;
}

template <class T>
void ConcurrentQueue<T>::Push(T &element) {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  internal_.emplace_back(element);
}

template <class T>
auto ConcurrentQueue<T>::Erase(u64 no_elements) -> bool {
  sync::HybridGuard guard(&latch_, sync::GuardMode::EXCLUSIVE);
  if (internal_.size() < no_elements) { return false; }
  internal_.erase(internal_.begin(), internal_.begin() + no_elements);
  return true;
}

template <class T>
auto ConcurrentQueue<T>::SizeApprox() -> size_t {
  size_t ret = 0;

  while (true) {
    try {
      sync::HybridGuard guard(&latch_, sync::GuardMode::OPTIMISTIC);
      ret = internal_.size();
      break;
    } catch (const sync::RestartException &) {}
  }

  return ret;
}

// ----------------------------------------------------------------------------------------------

template class ConcurrentQueue<transaction::Transaction>;
template class LockFreeQueue<transaction::SerializableTransaction>;
template void LockFreeQueue<transaction::SerializableTransaction>::Push_DR<leanstore::transaction::Transaction>(
  const leanstore::transaction::Transaction &);

}  // namespace leanstore