#pragma once
// Where the engine's files are. A packaged engine (linux/package.sh) keeps
// its shaders in <root>/share/vrx/shaders and the model in
// <root>/share/vrx/models beside <root>/bin/vrx-engine; a build keeps them
// at the paths CMake compiles in. An environment override beats both.
#include <string>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>

namespace vrx {

inline bool FileExists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

// <root>/share/vrx when this executable lives in <root>/bin, else empty.
inline std::string PackageShareDirectory() {
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) return {};
    std::string exe(buffer, size_t(length));
    const auto slash = exe.rfind('/');
    if (slash == std::string::npos) return {};
    const std::string bin = exe.substr(0, slash);
    const auto parent = bin.rfind('/');
    if (parent == std::string::npos || bin.compare(parent + 1, std::string::npos, "bin") != 0) return {};
    return bin.substr(0, parent) + "/share/vrx";
}

// The env override, else the packaged file, else the build's path.
inline std::string ResourcePath(const char* environment, const char* subdirectory, const char* file,
                                const char* builtIn) {
    if (const char* value = std::getenv(environment); value && *value) return value;
    const std::string share = PackageShareDirectory();
    if (!share.empty()) {
        const std::string packaged = share + "/" + subdirectory + "/" + file;
        if (FileExists(packaged)) return packaged;
    }
    return builtIn ? builtIn : file;
}

inline std::string ShaderPath(const char* environment, const char* file, const char* builtIn) {
    return ResourcePath(environment, "shaders", file, builtIn);
}

} // namespace vrx
