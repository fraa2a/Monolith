#pragma once
#include <nlohmann/json.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif
namespace recording {
struct Bookmark {
    double time_seconds = 0;
    std::string label;
    std::string color;
};
inline std::filesystem::path bookmark_journal_path(const std::wstring& media) {
    return std::filesystem::path(media + L".bookmarks.json");
}
inline bool write_bookmark_journal(const std::wstring& media,
    const std::vector<Bookmark>& bookmarks, std::string* error = nullptr) {
    try {
        if (bookmarks.size() > 4096) throw std::runtime_error("Too many bookmarks");
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& b : bookmarks) {
            if (!std::isfinite(b.time_seconds) || b.time_seconds < 0)
                throw std::runtime_error("Invalid bookmark time");
            rows.push_back({{"time_seconds", b.time_seconds}, {"label", b.label}, {"color", b.color}});
        }
        const auto text = nlohmann::json{{"version", 1}, {"bookmarks", rows}}.dump();
        if (text.size() > 1024 * 1024) throw std::runtime_error("Bookmark journal too large");
        const auto path = bookmark_journal_path(media);
        const auto temp = std::filesystem::path(media + L".bookmarks.pending.tmp");
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
            std::filesystem::is_symlink(std::filesystem::symlink_status(temp)))
            throw std::runtime_error("Bookmark journal symlink rejected");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out) throw std::runtime_error("Bookmark journal write failed");
        }
#ifdef _WIN32
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Bookmark journal replace failed");
#else
        std::filesystem::rename(temp, path);
#endif
        return true;
    } catch (const std::exception& e) { if (error) *error = e.what(); return false; }
}
inline bool read_bookmark_journal(const std::wstring& media,
    std::vector<Bookmark>& bookmarks, std::string* error = nullptr) {
    try {
        const auto path = bookmark_journal_path(media);
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)) ||
            !std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 1024 * 1024)
            throw std::runtime_error("Invalid bookmark journal file");
        std::ifstream in(path, std::ios::binary);
        const auto json = nlohmann::json::parse(in);
        if (json.at("version") != 1 || !json.at("bookmarks").is_array() ||
            json.at("bookmarks").size() > 4096) throw std::runtime_error("Invalid bookmark journal schema");
        std::vector<Bookmark> rows;
        for (const auto& item : json.at("bookmarks")) {
            Bookmark b{item.at("time_seconds").get<double>(), item.at("label").get<std::string>(),
                item.at("color").get<std::string>()};
            if (!std::isfinite(b.time_seconds) || b.time_seconds < 0)
                throw std::runtime_error("Invalid bookmark time");
            rows.push_back(std::move(b));
        }
        bookmarks = std::move(rows);
        return true;
    } catch (const std::exception& e) { if (error) *error = e.what(); return false; }
}
}
