#include "debug_paths.h"

#include "modules/Filesystem.h"

#include <filesystem>

using namespace DFHack;

std::string smoothpan_log_path(const char* filename) {
    namespace fs = std::filesystem;

    try {
        fs::path dir = Filesystem::getInstallDir() / "dfhack-config" / "smoothpan";
        fs::create_directories(dir);
        return (dir / filename).string();
    } catch (...) {
    }

    const char* fallbacks[] = {
        "D:\\dfhack\\smoothpan",
        "D:\\dfhack",
    };
    for (const char* fb : fallbacks) {
        try {
            fs::path dir(fb);
            fs::create_directories(dir);
            return (dir / filename).string();
        } catch (...) {
        }
    }

    return filename;
}
