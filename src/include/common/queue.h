#pragma once

#include "common/exceptions.h"
#include "common/typedefs.h"
#include "common/utils.h"
#include "sync/hybrid_latch.h"

#include "gtest/gtest_prod.h"

#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <limits>
#include <type_traits>

namespace leanstore {
template <typename T>
class LockFreeQueue {
 public:
  static constexpr uoffset_t NULL_SIZE = std::numeric_limits<uoffset_t>::max();

  LockFreeQueue();
  ~LockFreeQueue();

  /**
   * @brief Push a serialized element to the queue from unserialized data
   */
  template <typename T2>
  void Push(const T2 &element) {
    auto item_size = static_cast<uoffset_t>(element.SerializedSize());
    auto w_tail    = tail_.load(std::memory_order_acquire);
    // std::printf("Item Size: %u bytes\n", item_size);

    /* Circular buffer: no room for this element + a CR entry, so we circular back */
    if (buffer_capacity_ - w_tail < item_size + sizeof(T::NULL_ITEM)) {
      Ensure(buffer_capacity_ - w_tail >= sizeof(T::NULL_ITEM));
      std::memcpy(&buffer_[w_tail], &(T::NULL_ITEM), sizeof(T::NULL_ITEM));
      w_tail = 0;
    }

    /* Wait until the consumer accesses more items to free up some memory */
    uint64_t yield_ns = 0, total_ns = 0;
    auto start  = std::chrono::high_resolution_clock::now();
    auto r_head = head_.load(std::memory_order_acquire);
    // std::cout << "Debug Push : NoSpace = " << NoSpace(r_head, w_tail, item_size) << std::endl;

    while (NoSpace(r_head, w_tail, item_size, no_txn_.load(std::memory_order_acquire))) {
      /*std::cout << "r_head = " << r_head << " w_tail = " << w_tail << " item_size = " << item_size
              << " no_txn_ = " << no_txn_ << std::endl;
      break;*/
      auto y_start = std::chrono::high_resolution_clock::now();
      r_head       = head_.load(std::memory_order_acquire);
      AsmYield();
      yield_ns +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - y_start)
          .count();
    }

    /* Have enough memory -> producer write the element to the buffer */
    Ensure(w_tail % CPU_CACHELINE_SIZE == 0);
    auto obj = reinterpret_cast<T *>(&buffer_[w_tail]);
    obj->Construct(element);
    no_txn_.fetch_add(1, std::memory_order_release);
    tail_.store(w_tail + item_size, std::memory_order_release);

    total_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::high_resolution_clock::now() - start).count();
    if (yield_ns > 0) { std::printf("Total: %lu ns | Yield: %lu ns\n", total_ns, yield_ns); }
  }

 public:
  struct QueueBlock {
    u8 *buffer;
    u64 buffer_capacity;
    std::atomic<u64> no_txn   = 0;
    std::atomic<u64> no_txn_b = 0;
    std::atomic<uint64_t> last_used;
    std::atomic<u64> head = {0}; /* Read from head */
    std::atomic<u64> tail = {0}; /* Write to tail */

    std::atomic<QueueBlock *> prev{nullptr};
    std::atomic<QueueBlock *> next{nullptr};

    QueueBlock() : buffer_capacity(FLAGS_txn_queue_size_mb * KB) {
      buffer = reinterpret_cast<u8 *>(AllocHuge(buffer_capacity));
    }

    ~QueueBlock() { munmap(buffer, buffer_capacity); }
  };

 private:
  FRIEND_TEST(TestQueue, BasicTest);
  FRIEND_TEST(TestQueue, ConcurrencyTest);

  u8 *buffer_;
  u64 buffer_capacity_;
  std::atomic<u64> head_   = {0}; /* Read from head */
  std::atomic<u64> tail_   = {0}; /* Write to tail */
  std::atomic<u64> no_txn_ = 0;

  std::atomic<QueueBlock *> current_read_block_;
  std::atomic<QueueBlock *> current_write_block_;
  std::atomic<QueueBlock *> first_block_;

  auto ContiguousFreeBytes(u64 r_head, u64 w_tail) -> u64 {
    // circulate the wal_cursor to the beginning and insert the whole entry
    return (w_tail < r_head) ? r_head - w_tail : buffer_capacity_ - w_tail;
  }

  auto NoSpace(u64 r_head, u64 w_tail, u64 space_needed, u64 no_txn) -> bool {
    if (w_tail < r_head) {
      return (r_head - w_tail) < space_needed;
    } else if (w_tail == r_head) {
      return no_txn > 0;
    } else {
      return false;
    }
  }

 public:
  constexpr auto CurrentTail() -> u64 { return tail_.load(std::memory_order_acquire); }

  constexpr auto CurrentTail_DR() -> u64 {
    return current_write_block_.load(std::memory_order_acquire)->tail.load(std::memory_order_acquire);
  }

  auto CurrentWriteBlock_DR() -> QueueBlock * { return current_write_block_.load(std::memory_order_acquire); }

  auto CurrentReadBlock_DR() -> QueueBlock * { return current_read_block_.load(std::memory_order_acquire); }

  /* Erase and loop utilities */
  void Erase(u64 no_bytes, u64 no_txn);
  auto LoopElements(u64 until_tail, const std::function<bool(T &)> &read_cb) -> std::tuple<u64, u64>;

  template <typename T2>
  void Push_DR(const T2 &element);
  void Erase_DR(u64 no_bytes, u64 read_txn, u64 read_txn_b, QueueBlock *new_r_block);
  auto LoopElements_DR(u64 until_tail, QueueBlock *tail_block, const std::function<bool(T &)> &read_cb)
    -> std::tuple<u64, u64, u64, QueueBlock *, u64>;
  auto Batch_Loop(u64 until_tail, QueueBlock *tail_block, const std::function<bool(T &)> &read_cb)
    -> std::tuple<u64, u64, u64, QueueBlock *, u64>;
};

template <class T>
class ConcurrentQueue {
 public:
  ConcurrentQueue()  = default;
  ~ConcurrentQueue() = default;

  /**
   * @brief Unsafe operator[], assuming that size of the internal deque is larger than the idx
   * Only used for testing
   */
  auto operator[](u64 idx) -> T & { return internal_[idx]; }

  auto LoopElement(u64 no_elements, const std::function<bool(T &)> &fn) -> u64;
  void Push(T &element);
  auto Erase(u64 no_elements) -> bool;
  auto SizeApprox() -> size_t;

 private:
  std::deque<T> internal_;
  sync::HybridLatch latch_;
};

}  // namespace leanstore