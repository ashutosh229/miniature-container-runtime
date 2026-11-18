#pragma once
// Filesystem Manager: rootfs validation (parent) and root switching (child).
#include <string>

namespace mc {

class FilesystemManager {
public:
    // Returns the canonical path. Throws if missing, not a directory, "/" or not root-fs-like.
    static std::string validate_rootfs(const std::string& path);
    // Checks the executable exists *inside* rootfs. Throws ExitError(127) otherwise.
    static void validate_command(const std::string& rootfs, const std::string& command);
    // Child side, inside the new mount namespace:
    //   private propagation -> bind rootfs -> pivot_root -> detach old root -> /proc, /dev, /tmp
    static void enter_root(const std::string& rootfs);
};

}  // namespace mc
