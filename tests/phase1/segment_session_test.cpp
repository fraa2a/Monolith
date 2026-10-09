#include <disk-segments/session_directory.h>
#include <cassert>
#include <fstream>
#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif
int main() {
    namespace fs = std::filesystem;
    auto root = fs::temp_directory_path() / "monolith-session-regression";
    fs::remove_all(root); fs::create_directories(root);
    {
        disk_segments::SessionDirectory live; assert(live.create(root));
        auto path = live.path(); std::ofstream(path / "seg_0.mkv") << "media";
        assert(disk_segments::SessionDirectory::reclaim(root)); assert(fs::exists(path / "seg_0.mkv"));
    }
#ifndef _WIN32
    const auto child = fork(); assert(child >= 0);
    if (child == 0) {
        disk_segments::SessionDirectory dead; if (!dead.create(root)) _exit(2);
        std::ofstream(dead.path() / "seg_1.mp4") << "orphan";
        std::ofstream(dead.path() / "personal.txt") << "keep"; _exit(0);
    }
    int status = 0; waitpid(child, &status, 0); assert(status == 0);
    assert(disk_segments::SessionDirectory::reclaim(root)); bool foreign = false;
    for (const auto& item : fs::recursive_directory_iterator(root)) {
        assert(item.path().filename() != "seg_1.mp4");
        if (item.path().filename() == "personal.txt") foreign = true;
    }
    assert(foreign);
#endif
    fs::remove_all(root);
}
