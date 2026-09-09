#pragma once
#include <cstdint>
#include <cstddef>
namespace disk_segments {
inline constexpr int kDefaultDiskBudgetMb = 2048;
inline constexpr int kMinDiskBudgetMb = 512;
inline constexpr int kMaxDiskBudgetMb = 65536;
inline constexpr uint64_t kMaxSegmentPayload = 256ULL * 1024 * 1024;
inline constexpr uint64_t kFreeSpaceReserve = 512ULL * 1024 * 1024;
inline constexpr size_t kMaxSegments = 4096;
inline int validated_disk_budget_mb(int64_t value) {
    return value >= kMinDiskBudgetMb && value <= kMaxDiskBudgetMb ? static_cast<int>(value) : kDefaultDiskBudgetMb;
}
} // namespace disk_segments
