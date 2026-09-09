#pragma once
#include <disk-segments/disk_budget.h>
#include <nlohmann/json.hpp>

namespace settings {
// Shared by load and focused regression tests. Validate JSON before narrowing:
// uint64 extremes, floats, strings, booleans and missing keys use the default.
inline int replay_disk_budget_mb_from_json(const nlohmann::json& doc) {
    if (doc.is_object() && doc.contains("replay_buffer") && doc["replay_buffer"].is_object()) {
        const auto& replay = doc["replay_buffer"];
        if (replay.contains("disk_budget_mb")) {
            const auto& value = replay["disk_budget_mb"];
            if (value.is_number_integer() && value >= disk_segments::kMinDiskBudgetMb &&
                value <= disk_segments::kMaxDiskBudgetMb) return value.get<int>();
        }
    }
    return disk_segments::kDefaultDiskBudgetMb;
}
} // namespace settings
