#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace settings {

struct Config {
    std::wstring user_config_path;
    std::wstring clips_directory;
    std::wstring recordings_directory;
    std::wstring temp_directory;
    int replay_duration_seconds = 30;
    int64_t replay_memory_budget_mb = 512;
    std::string merged_json;
};

struct LoadResult {
    Config config;
    std::vector<std::string> warnings;
};

LoadResult load(
    const std::wstring& app_data_dir,
    const std::wstring& default_clips_directory,
    const std::wstring& default_recordings_directory,
    const std::wstring& default_temp_directory);

bool save(Config& config, std::string* error);

} // namespace settings
