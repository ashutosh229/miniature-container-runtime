#include "filesystem/filesystem_manager.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <stdexcept>

#include "common/util.hpp"

namespace mc {
namespace {

void do_mount(const char* src, const char* target, const char* fstype, unsigned long flags, const char* data,
              const std::string& what) {
    if (::mount(src, target, fstype, flags, data) != 0) throw SysError("mount " + what);
}

void mkdir_ok(const char* path, mode_t mode) {
    if (::mkdir(path, mode) != 0 && errno != EEXIST) throw SysError(std::string("mkdir ") + path);
}

void populate_dev() {
    mkdir_ok("/dev", 0755);
    do_mount("tmpfs", "/dev", "tmpfs", MS_NOSUID | MS_STRICTATIME, "mode=755,size=65536", "tmpfs on /dev");
    struct Dev { const char* path; mode_t mode; unsigned maj, min; };
    const Dev devs[] = {{"/dev/null", 0666, 1, 3},   {"/dev/zero", 0666, 1, 5},    {"/dev/full", 0666, 1, 7},
                        {"/dev/random", 0666, 1, 8}, {"/dev/urandom", 0666, 1, 9}, {"/dev/tty", 0666, 5, 0}};
    mode_t old = ::umask(0);
    for (const auto& d : devs)
        if (::mknod(d.path, S_IFCHR | d.mode, makedev(d.maj, d.min)) != 0) {
            int e = errno;
            ::umask(old);
            throw SysError(std::string("mknod ") + d.path, e);
        }
    ::umask(old);
    (void)!::symlink("/proc/self/fd", "/dev/fd");
    (void)!::symlink("/proc/self/fd/0", "/dev/stdin");
    (void)!::symlink("/proc/self/fd/1", "/dev/stdout");
    (void)!::symlink("/proc/self/fd/2", "/dev/stderr");
    mkdir_ok("/dev/shm", 01777);
    ::mount("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777,size=16m");  // best effort
}

}  // namespace

std::string FilesystemManager::validate_rootfs(const std::string& path) {
    if (path.empty()) throw std::invalid_argument("--rootfs is required");
    char buf[PATH_MAX];
    if (!::realpath(path.c_str(), buf)) throw SysError("rootfs '" + path + "'");
    struct stat sb;
    if (::stat(buf, &sb) != 0 || !S_ISDIR(sb.st_mode)) throw std::runtime_error("rootfs '" + path + "' is not a directory");
    const std::string real = buf;
    if (real == "/") throw std::runtime_error("refusing to use '/' as a container rootfs (it would expose the host)");
    if (!path_exists(real + "/bin") && !path_exists(real + "/usr/bin"))
        throw std::runtime_error("'" + real + "' does not look like a root filesystem (no /bin or /usr/bin); run scripts/setup_rootfs.sh");
    return real;
}

void FilesystemManager::validate_command(const std::string& rootfs, const std::string& cmd) {
    if (cmd.empty()) throw std::invalid_argument("no command given");
    auto exists = [&](const std::string& rel) {
        struct stat sb;
        const std::string p = rootfs + rel;
        return ::lstat(p.c_str(), &sb) == 0 && !S_ISDIR(sb.st_mode);
    };
    if (cmd.find('/') != std::string::npos) {
        if (cmd[0] != '/') throw std::invalid_argument("a command containing '/' must be an absolute path inside the rootfs");
        if (!exists(cmd)) throw ExitError(127, "command '" + cmd + "' not found inside rootfs " + rootfs);
        return;
    }
    for (const char* dir : {"/usr/local/sbin", "/usr/local/bin", "/usr/sbin", "/usr/bin", "/sbin", "/bin"})
        if (exists(std::string(dir) + "/" + cmd)) return;
    throw ExitError(127, "command '" + cmd + "' not found in the rootfs PATH");
}

void FilesystemManager::enter_root(const std::string& rootfs) {
    // 1. Stop mount events from propagating back to the host's mount namespace.
    do_mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr, "/ (make rprivate)");
    // 2. pivot_root requires the new root to be a mount point: bind it onto itself.
    do_mount(rootfs.c_str(), rootfs.c_str(), nullptr, MS_BIND | MS_REC, nullptr, "bind " + rootfs);
    // 3. Place for the old root, inside the new root.
    const std::string put_old = rootfs + "/.pivot_old";
    if (::mkdir(put_old.c_str(), 0700) != 0 && errno != EEXIST) throw SysError("mkdir " + put_old);
    if (::chdir(rootfs.c_str()) != 0) throw SysError("chdir " + rootfs);
    // 4. Swap roots.
    if (::syscall(SYS_pivot_root, ".", ".pivot_old") != 0) throw SysError("pivot_root");
    if (::chdir("/") != 0) throw SysError("chdir /");
    // 5. Detach the old root so the host filesystem is unreachable, then remove the mount point.
    if (::umount2("/.pivot_old", MNT_DETACH) != 0) throw SysError("umount2 /.pivot_old");
    ::rmdir("/.pivot_old");
    // 6. /proc must be mounted AFTER we are inside the new PID namespace so it shows only our processes.
    mkdir_ok("/proc", 0555);
    do_mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, nullptr, "proc on /proc");
    populate_dev();
    mkdir_ok("/tmp", 01777);
    ::mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");  // best effort
    ::umask(022);
}

}  // namespace mc
