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
LockFreeQueue<T>::LockFreeQueue() : buffer_capacity_(FLAGS_txn_queue_size_kb * KB) {
  assert(std::is_trivially_destructible_v<T>);
  if (FLAGS_dynamic_resizing) {
    first_block_.store(new QueueBlock(), std::memory_order_release);
    first_block_.load(std::memory_order_acquire)->last_used.store(tsctime::ReadTSC(), std::memory_order_release);
    current_write_block_.store(first_block_.load(std::memory_order_acquire), std::memory_order_release);
    current_read_block_.store(first_block_.load(std::memory_order_acquire), std::memory_order_release);
  } else {
    buffer_ = reinterpret_cast<u8 *>(AllocHuge(buffer_capacity_));
  }
}

template <typename T>
LockFreeQueue<T>::~LockFreeQueue() {
  if (FLAGS_dynamic_resizing) {
    auto current = first_block_.load(std::memory_order_acquire);
    while (current != nullptr) {
      auto next = current->next.load(std::memory_order_acquire);
      delete current;
      current = next;
    }

  } else {
    munmap(buffer_, buffer_capacity_);
  }
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
  if (no_bytes <= 0) { return; }
  auto r_head = head_.load(std::memory_order_acquire);

  if (r_head + no_bytes < buffer_capacity_) {
    r_head += no_bytes;
  } else {
    r_head = no_bytes - (buffer_capacity_ - r_head);
  }
  head_.store(r_head, std::memory_order_release);
  no_txn_.fetch_add(-no_txn, std::memory_order_release);
}

/**
 * @brief Push a serialized element to the queue from unserialized data with dynamic resizing
 */

template <typename T>
template <typename T2>
void LockFreeQueue<T>::Push_DR(const T2 &element) {
  auto item_size = static_cast<uoffset_t>(element.SerializedSize());
  auto w_block   = current_write_block_.load(std::memory_order_acquire);
  auto w_tail    = w_block->tail.load(std::memory_order_acquire);

  /* Circular buffer: no room for this element + a CR entry, so we circular back */
  if (w_block->buffer_capacity - w_tail < item_size + sizeof(T::NULL_ITEM)) {
    Ensure(w_block->buffer_capacity - w_tail >= sizeof(T::NULL_ITEM));
    std::memcpy(&w_block->buffer[w_tail], &(T::NULL_ITEM), sizeof(T::NULL_ITEM));
    w_tail = 0;
  }

  /* Allocate new queueblock */
  auto r_head_w_block = w_block->head.load(std::memory_order_acquire);
  if (NoSpace(r_head_w_block, w_tail, (item_size + sizeof(T::JUMP_ITEM)),
              w_block->no_txn.load(std::memory_order_acquire))) {
    if (w_block->next.load(std::memory_order_acquire) == nullptr) {
      QueueBlock *new_block = new QueueBlock();
      QueueBlock *expected  = nullptr;
      if (w_block->next.compare_exchange_strong(expected, new_block, std::memory_order_acq_rel)) {
        new_block->prev.store(w_block, std::memory_order_release);
      } else {
        delete new_block;
      }
    }
    current_write_block_.store(w_block->next.load(std::memory_order_acquire), std::memory_order_release);
    std::memcpy(&w_block->buffer[w_tail], &(T::JUMP_ITEM), sizeof(T::JUMP_ITEM));
    w_block = current_write_block_.load(std::memory_order_acquire);
    w_tail  = w_block->tail.load(std::memory_order_acquire);
  }

  /* Have enough memory -> producer write the element to the buffer */
  Ensure(w_tail % CPU_CACHELINE_SIZE == 0);
  auto obj = reinterpret_cast<T *>(&w_block->buffer[w_tail]);
  obj->Construct(element);
  w_block->tail.store(w_tail + item_size, std::memory_order_release);
  w_block->no_txn.fetch_add(1, std::memory_order_release);
  if (element.state == transaction::Transaction::State::BARRIER) {
    w_block->no_txn_b.fetch_add(1, std::memory_order_release);
  }

  auto commit_stats = tsctime::ReadTSC();
  w_block->last_used.store(commit_stats, std::memory_order_release);
  if (w_block->next.load(std::memory_order_acquire) != nullptr) {
    auto block_tbr = w_block->next.load(std::memory_order_acquire);
    while (tsctime::TscDifferenceNs(block_tbr->last_used.load(std::memory_order_acquire), commit_stats) >=
           FLAGS_queueblock_removal_threshold * 1000000000ULL) {
      Ensure(block_tbr->head.load(std::memory_order_acquire) == 0 &&
             block_tbr->no_txn.load(std::memory_order_acquire) == 0);
      auto block_tbr_next = block_tbr->next.load(std::memory_order_acquire);
      if (block_tbr->prev.load(std::memory_order_acquire)
            ->next.compare_exchange_strong(block_tbr, block_tbr_next, std::memory_order_acq_rel)) {
        if (block_tbr_next != nullptr) {
          block_tbr_next->prev.store(block_tbr->prev.load(std::memory_order_acquire), std::memory_order_release);
        }
        delete block_tbr;
        block_tbr = block_tbr_next;
        if (block_tbr == nullptr) { break; }
      } else {
        break;
      }
    }
  }
}

template <typename T>
void LockFreeQueue<T>::Erase_DR(u64 no_bytes, u64 read_txn, u64 read_txn_b, QueueBlock *new_r_block) {
  auto r_block = current_read_block_.load(std::memory_order_acquire);
  auto r_head  = r_block->head.load(std::memory_order_acquire);

  if (r_block != new_r_block) {
    auto current = r_block;
    while (current != new_r_block) {
      current->no_txn.store(0, std::memory_order_release);
      current->no_txn_b.store(0, std::memory_order_release);
      current->last_used.store(tsctime::ReadTSC(), std::memory_order_release);
      current->head.store(0, std::memory_order_release);
      current->tail.store(0, std::memory_order_release);
      current = current->next.load(std::memory_order_acquire);
    }

    while (current->next.load(std::memory_order_acquire) != nullptr) {
      current = current->next.load(std::memory_order_acquire);
    }

    QueueBlock *expected = nullptr;

    while (!current->next.compare_exchange_strong(expected, r_block)) {
      current  = expected;
      expected = nullptr;
    }

    if (first_block_.load(std::memory_order_acquire) == r_block) {
      first_block_.store(new_r_block, std::memory_order_release);
    }

    if (r_block->prev.load(std::memory_order_acquire) != nullptr) {
      r_block->prev.load(std::memory_order_acquire)->next.store(new_r_block, std::memory_order_release);
    }

    new_r_block->prev.load(std::memory_order_acquire)->next.store(nullptr, std::memory_order_release);
    new_r_block->prev.store(r_block->prev.load(std::memory_order_acquire), std::memory_order_release);

    r_block->prev.store(current, std::memory_order_release);
    r_block = new_r_block;
    r_head  = r_block->head.load(std::memory_order_acquire);
  }

  if (r_head + no_bytes < r_block->buffer_capacity) {
    r_head += no_bytes;
  } else {
    r_head = no_bytes - (r_block->buffer_capacity - r_head);
  }
  r_block->no_txn.fetch_add(-read_txn, std::memory_order_release);
  r_block->no_txn_b.fetch_add(-read_txn_b, std::memory_order_release);
  current_read_block_.store(r_block, std::memory_order_release);
  r_block->head.store(r_head, std::memory_order_release);
  r_block->last_used.store(tsctime::ReadTSC(), std::memory_order_release);
}

template <typename T>
auto LockFreeQueue<T>::LoopElements_DR(u64 until_tail, QueueBlock *tail_block, const std::function<bool(T &)> &read_cb)
  -> std::tuple<u64, u64, u64, QueueBlock *, u64> {
  auto r_block      = current_read_block_.load(std::memory_order_acquire);
  auto r_head       = r_block->head.load(std::memory_order_acquire);
  u64 no_bytes      = 0;
  u64 read_txn      = 0;
  u64 read_txn_b    = 0;
  u64 committed_txn = 0;

  while (!(r_block == tail_block && r_head == until_tail)) {
    if (T::InvalidByteBuffer(&r_block->buffer[r_head])) {
      no_bytes += r_block->buffer_capacity - r_head;
      r_head = 0;
    }
    if (T::JumpByteBuffer(&r_block->buffer[r_head])) {
      //std::cout << "Jump byte" << std::endl;
      r_block->last_used.store(tsctime::ReadTSC(), std::memory_order_release);
      r_block    = r_block->next.load(std::memory_order_acquire);
      r_head     = r_block->head.load(std::memory_order_acquire);
      no_bytes   = 0;
      read_txn   = 0;
      read_txn_b = 0;
      continue;
    }
    const auto item = reinterpret_cast<T *>(&r_block->buffer[r_head]);

    if (!read_cb(*item)) { break; }
    no_bytes += item->MemorySize();
    read_txn++;
    if (item->state != transaction::Transaction::State::BARRIER) { committed_txn++; }
    if (item->state == transaction::Transaction::State::BARRIER) { read_txn_b++; }
    r_head += item->MemorySize();
  }

  r_block->last_used.store(tsctime::ReadTSC(), std::memory_order_release);

  return std::make_tuple(no_bytes, read_txn, read_txn_b, r_block, committed_txn);
}

template <typename T>
auto LockFreeQueue<T>::Batch_Loop(u64 until_tail, QueueBlock *tail_block, const std::function<bool(T &)> &read_cb)
  -> std::tuple<u64, u64, u64, QueueBlock *, u64> {
  auto r_block         = current_read_block_.load(std::memory_order_acquire);
  auto current         = r_block;
  auto r_head          = r_block->head.load(std::memory_order_acquire);
  u64 no_bytes         = 0;
  u64 read_txn         = 0;
  u64 read_txn_b       = 0;
  u64 committed_txn_wb = 0;
  u64 committed_txn_b  = 0;

  if (r_block != tail_block) {
    while (true) {
      if (T::InvalidByteBuffer(&r_block->buffer[r_head])) { r_head = 0; }
      if (T::JumpByteBuffer(&r_block->buffer[r_head])) {
        if (r_block == tail_block || r_block->next.load(std::memory_order_acquire) == nullptr) {
          if (r_block->prev.load(std::memory_order_acquire) != nullptr) {
            r_block = r_block->prev.load(std::memory_order_acquire);
            r_head  = r_block->head.load(std::memory_order_acquire);
          }
          break;
        }
        r_block = r_block->next.load(std::memory_order_acquire);
        r_head  = r_block->head.load(std::memory_order_acquire);
        continue;
      }
      const auto item = reinterpret_cast<T *>(&r_block->buffer[r_head]);
      if (!read_cb(*item)) {
        if (r_block->prev.load(std::memory_order_acquire) != nullptr) {
          r_block = r_block->prev.load(std::memory_order_acquire);
          r_head  = r_block->head.load(std::memory_order_acquire);
        }
        break;
      }
      if (r_block == tail_block || r_block->next.load(std::memory_order_acquire) == nullptr) { break; }
      r_block = r_block->next.load(std::memory_order_acquire);
      r_head  = r_block->head.load(std::memory_order_acquire);
    }

    auto blocks_looped = 0;
    while (current != r_block) {
      committed_txn_wb += current->no_txn.load(std::memory_order_acquire);
      committed_txn_b += current->no_txn_b.load(std::memory_order_acquire);
      blocks_looped++;
      current = current->next.load(std::memory_order_acquire);
    }
    //if (blocks_looped > 0) { std::cout << "Txn looped = " << committed_txn_wb << std::endl; }
  }

  while (!(r_block == tail_block && r_head == until_tail)) {
    if (T::InvalidByteBuffer(&r_block->buffer[r_head])) {
      no_bytes += r_block->buffer_capacity - r_head;
      r_head = 0;
    }
    if (T::JumpByteBuffer(&r_block->buffer[r_head])) {
      //std::cout << "Jump byte" << std::endl;
      r_block->last_used.store(tsctime::ReadTSC(), std::memory_order_release);
      r_block    = r_block->next.load(std::memory_order_acquire);
      r_head     = r_block->head.load(std::memory_order_acquire);
      no_bytes   = 0;
      read_txn   = 0;
      read_txn_b = 0;
      continue;
    }
    const auto item = reinterpret_cast<T *>(&r_block->buffer[r_head]);

    if (!read_cb(*item)) { break; }
    committed_txn_wb++;
    no_bytes += item->MemorySize();
    read_txn++;
    if (item->state == transaction::Transaction::State::BARRIER) {
      read_txn_b++;
      committed_txn_b++;
    }
    r_head += item->MemorySize();
  }

  r_block->last_used.store(tsctime::ReadTSC(), std::memory_order_release);

  return std::make_tuple(no_bytes, read_txn, read_txn_b, r_block, committed_txn_wb - committed_txn_b);
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
  auto r_head   = head_.load(std::memory_order_acquire);
  auto old_head = r_head;
  auto no_txn   = 0;

  for (auto idx = 0UL; r_head != until_tail; idx++) {
    assert(r_head != until_tail);
    /* Circular back to the beginning of the buffer if deadend meet */
    if (T::InvalidByteBuffer(&buffer_[r_head])) { r_head = 0; }

    /* Read the queued item */
    const auto item = reinterpret_cast<T *>(&buffer_[r_head]);
    if (!read_cb(*item)) { break; }
    r_head += item->MemorySize();
    no_txn++;
  }

  return std::make_tuple(((r_head > old_head) ? r_head - old_head : r_head + buffer_capacity_ - old_head), no_txn);
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