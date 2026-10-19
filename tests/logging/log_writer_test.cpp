#include <logging/logging.h>
#include <logging/log_writer.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

static std::string read(const std::filesystem::path& path) {
    std::ifstream in(path);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int main() {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / ("monolith-log-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    auto capture = root / "stderr.txt";
    FILE* output = std::fopen(capture.c_str(), "wb");
    assert(output);
    const int old_stderr = dup(fileno(stderr));
    assert(old_stderr >= 0 && dup2(fileno(output), fileno(stderr)) >= 0);
    logging::init(false, (root / "missing").wstring());
    logging::log_error("test", "error-details-must-survive-unavailable-file");
    logging::init(false, root.wstring());
    {
        logging::LogWriter writer("/dev/full");
        assert(writer.write("error-details-must-survive-flush-failure\n"));
    }
    {
        logging::LogWriter writer("/dev/full");
        assert(writer.write(std::string(8192, 'x') + "error-details-must-survive-write-failure\n"));
    }
    std::fflush(stderr);
    assert(dup2(old_stderr, fileno(stderr)) >= 0);
    close(old_stderr);
    std::fclose(output);
    const auto fallback = read(capture);
    assert(fallback.find("error-details-must-survive-unavailable-file") != std::string::npos);
    assert(fallback.find("error-details-must-survive-flush-failure") != std::string::npos);
    assert(fallback.find("error-details-must-survive-write-failure") != std::string::npos);
    logging::log("test", "verbose-must-not-appear");
    logging::log_error("test", "error-must-persist");
    logging::init(false, (root / "unused").wstring());
    const auto logged = read(root / "monolith.log");
    assert(logged.find("error-must-persist") != std::string::npos);
    assert(logged.find("verbose-must-not-appear") == std::string::npos);
    fs::create_directories(root / "rotation");
    {
        logging::LogWriter writer(root / "rotation" / "monolith.log", 64, 3, 4096);
        for (int i = 0; i < 12; ++i)
            assert(writer.write("record-" + std::to_string(i) + "-abcdefghijk\n"));
    }
    assert(fs::exists(root / "rotation" / "monolith.log.1"));
    assert(!fs::exists(root / "rotation" / "monolith.log.4"));
    for (const auto& file : fs::directory_iterator(root / "rotation")) assert(file.file_size() <= 64);
    assert(read(root / "rotation" / "monolith.log").find("record-11-") != std::string::npos);
    {
        logging::LogWriter writer(root / "bounded.log", 64, 3, 0);
        assert(!writer.write("rejected\n"));
    }
    fs::remove_all(root);
}
