#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>

namespace microfs {

class Histogram {
 public:
  static constexpr std::size_t kSubBuckets = 256;
  static constexpr std::size_t kBuckets = 64 * kSubBuckets;

  void Record(std::uint64_t value) {
    ++counts_[Index(value)];
    ++count_;
    if (value > max_) max_ = value;
  }

  void Reset() {
    counts_.fill(0);
    count_ = 0;
    max_ = 0;
  }

  [[nodiscard]] std::uint64_t count() const { return count_; }
  [[nodiscard]] std::uint64_t max() const { return max_; }

  [[nodiscard]] std::uint64_t Percentile(double percentile) const {
    if (count_ == 0) return 0;
    if (percentile < 0.0) percentile = 0.0;
    if (percentile > 100.0) percentile = 100.0;
    if (percentile == 100.0) return max_;
    const auto rank = static_cast<std::uint64_t>(
        (percentile / 100.0) * static_cast<double>(count_ - 1));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < counts_.size(); ++i) {
      seen += counts_[i];
      if (seen > rank) return UpperBound(i);
    }
    return max_;
  }

 private:
  static std::size_t Index(std::uint64_t value) {
    if (value < kSubBuckets) return static_cast<std::size_t>(value);
    const unsigned exponent = std::bit_width(value) - 1U;
    const unsigned shift = exponent - 8U;
    const auto mantissa =
        static_cast<std::size_t>((value >> shift) & (kSubBuckets - 1));
    return static_cast<std::size_t>(exponent) * kSubBuckets + mantissa;
  }

  static std::uint64_t UpperBound(std::size_t index) {
    if (index < kSubBuckets) return index;
    const unsigned exponent =
        static_cast<unsigned>(index / kSubBuckets);
    const unsigned mantissa =
        static_cast<unsigned>(index % kSubBuckets);
    const unsigned shift = exponent - 8U;
    return (std::uint64_t{1} << exponent) +
           (static_cast<std::uint64_t>(mantissa + 1) << shift) - 1;
  }

  std::array<std::uint64_t, kBuckets> counts_{};
  std::uint64_t count_ = 0;
  std::uint64_t max_ = 0;
};

}  // namespace microfs
