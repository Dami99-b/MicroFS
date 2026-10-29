#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <string_view>

#include "microfs/arena.h"
#include "microfs/status.h"

namespace microfs {

struct LexicographicComparator {
  int operator()(std::string_view lhs, std::string_view rhs) const {
    const std::size_t common = lhs.size() < rhs.size() ? lhs.size() : rhs.size();
    for (std::size_t i = 0; i < common; ++i) {
      const auto a = static_cast<unsigned char>(lhs[i]);
      const auto b = static_cast<unsigned char>(rhs[i]);
      if (a < b) return -1;
      if (a > b) return 1;
    }
    if (lhs.size() < rhs.size()) return -1;
    if (lhs.size() > rhs.size()) return 1;
    return 0;
  }
};

template <typename Comparator = LexicographicComparator>
class SkipList {
 private:
  static constexpr std::size_t kMaxHeight = 12;

  struct Node {
    std::string_view key;
    std::string_view value;
    std::size_t height = 0;
    std::array<std::atomic<Node*>, kMaxHeight> next;

    Node() {
      for (auto& pointer : next) pointer.store(nullptr, std::memory_order_relaxed);
    }
  };

 public:
  class Iterator {
   public:
    explicit Iterator(const SkipList* list) : list_(list) {}

    [[nodiscard]] bool Valid() const { return node_ != nullptr; }
    [[nodiscard]] std::string_view key() const {
      return Valid() ? node_->key : std::string_view();
    }
    [[nodiscard]] std::string_view value() const {
      return Valid() ? node_->value : std::string_view();
    }

    void Seek(std::string_view target) {
      node_ = list_->FindGreaterOrEqual(target);
    }

    void SeekToFirst() {
      node_ = list_->head_->next[0].load(std::memory_order_acquire);
    }

    void SeekToLast() {
      node_ = list_->tail_.load(std::memory_order_acquire);
      if (node_ == list_->head_) node_ = nullptr;
    }

    void Next() {
      if (Valid()) node_ = node_->next[0].load(std::memory_order_acquire);
    }

    void Prev() {
      if (Valid()) node_ = list_->FindLessThan(node_->key);
    }

   private:
    const SkipList* list_;
    Node* node_ = nullptr;
  };

  explicit SkipList(Comparator comparator = Comparator(),
                    std::size_t arena_block_size = 4096)
      : arena_(arena_block_size), comparator_(comparator) {
    void* memory = arena_.Allocate(sizeof(Node), alignof(Node));
    if (memory == nullptr) {
      status_ = Status::ResourceExhausted("allocating skiplist head");
      return;
    }
    head_ = new (memory) Node();
    head_->height = kMaxHeight;
    tail_.store(head_, std::memory_order_relaxed);
  }

  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  [[nodiscard]] const Status& status() const { return status_; }
  [[nodiscard]] std::size_t allocated_bytes() const {
    return arena_.allocated_bytes();
  }

  Status Insert(std::string_view key, std::string_view value) {
    if (!status_.ok()) return status_;
    std::array<Node*, kMaxHeight> previous{};
    Node* candidate = FindGreaterOrEqual(key, &previous);
    if (candidate != nullptr && comparator_(candidate->key, key) == 0) {
      return Status::InvalidArgument("duplicate skiplist key");
    }

    const std::size_t height = RandomHeight();
    const std::size_t key_bytes = key.empty() ? 0 : key.size();
    const std::size_t value_bytes = value.empty() ? 0 : value.size();
    auto* node_memory =
        static_cast<std::byte*>(arena_.Allocate(sizeof(Node), alignof(Node)));
    if (node_memory == nullptr) {
      return Status::ResourceExhausted("allocating skiplist node");
    }
    char* key_copy = nullptr;
    if (key_bytes != 0) {
      key_copy = static_cast<char*>(arena_.Allocate(key_bytes, alignof(char)));
      if (key_copy == nullptr) {
        return Status::ResourceExhausted("allocating skiplist key");
      }
    }
    char* value_copy = nullptr;
    if (value_bytes != 0) {
      value_copy =
          static_cast<char*>(arena_.Allocate(value_bytes, alignof(char)));
      if (value_copy == nullptr) {
        return Status::ResourceExhausted("allocating skiplist value");
      }
    }
    if (key_bytes != 0) {
      std::copy(key.begin(), key.end(), key_copy);
    }
    if (value_bytes != 0) {
      std::copy(value.begin(), value.end(), value_copy);
    }
    Node* node = new (node_memory) Node();
    node->key = key_bytes == 0 ? std::string_view()
                               : std::string_view(key_copy, key_bytes);
    node->value = value_bytes == 0
                      ? std::string_view()
                      : std::string_view(value_copy, value_bytes);
    node->height = height;

    for (std::size_t level = 0; level < height; ++level) {
      Node* next = previous[level]->next[level].load(std::memory_order_relaxed);
      node->next[level].store(next, std::memory_order_relaxed);
      previous[level]->next[level].store(node, std::memory_order_release);
    }
    if (node->next[0].load(std::memory_order_relaxed) == nullptr) {
      tail_.store(node, std::memory_order_release);
    }
    return Status::OK();
  }

  [[nodiscard]] Node* FindGreaterOrEqual(std::string_view key) const {
    return FindGreaterOrEqual(key, nullptr);
  }

  [[nodiscard]] Iterator NewIterator() const { return Iterator(this); }

 private:
  Node* FindGreaterOrEqual(
      std::string_view key,
      std::array<Node*, kMaxHeight>* previous) const {
    Node* current = head_;
    for (std::size_t level = kMaxHeight; level-- > 0;) {
      Node* next = current->next[level].load(std::memory_order_acquire);
      while (next != nullptr && comparator_(next->key, key) < 0) {
        current = next;
        next = current->next[level].load(std::memory_order_acquire);
      }
      if (previous != nullptr) (*previous)[level] = current;
    }
    Node* candidate = current->next[0].load(std::memory_order_acquire);
    while (candidate != nullptr && comparator_(candidate->key, key) < 0) {
      candidate = candidate->next[0].load(std::memory_order_acquire);
    }
    return candidate;
  }

  Node* FindLessThan(std::string_view key) const {
    Node* current = head_;
    for (std::size_t level = kMaxHeight; level-- > 0;) {
      Node* next = current->next[level].load(std::memory_order_acquire);
      while (next != nullptr && comparator_(next->key, key) < 0) {
        current = next;
        next = current->next[level].load(std::memory_order_acquire);
      }
    }
    return current == head_ ? nullptr : current;
  }

  std::size_t RandomHeight() {
    std::size_t height = 1;
    while (true) {
      random_state_ ^= random_state_ << 13U;
      random_state_ ^= random_state_ >> 17U;
      random_state_ ^= random_state_ << 5U;
      if ((random_state_ & 3U) != 0 || height == kMaxHeight) break;
      ++height;
    }
    return height;
  }

  Arena arena_;
  Comparator comparator_;
  Node* head_ = nullptr;
  std::atomic<Node*> tail_{nullptr};
  std::uint32_t random_state_ = 0x9e3779b9U;
  Status status_;
};

}  // namespace microfs
