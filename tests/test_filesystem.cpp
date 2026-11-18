#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>

#include "common/util.hpp"
#include "filesystem/filesystem_manager.hpp"
#include "test_framework.hpp"

using namespace mc;

int main() {
    char tmpl[] = "/tmp/mc-fs-test-XXXXXX";
    std::string root = mkdtemp(tmpl);

    CHECK_THROWS(FilesystemManager::validate_rootfs(""));
    CHECK_THROWS(FilesystemManager::validate_rootfs("/definitely/not/here"));
    CHECK_THROWS(FilesystemManager::validate_rootfs("/"));       // would expose the host
    CHECK_THROWS(FilesystemManager::validate_rootfs(root));       // empty dir, no /bin

    mkdir((root + "/bin").c_str(), 0755);
    std::string real = FilesystemManager::validate_rootfs(root);  // now accepted
    CHECK(!real.empty());

    CHECK_THROWS(FilesystemManager::validate_command(real, "/bin/nope"));
    CHECK_THROWS(FilesystemManager::validate_command(real, "nope"));
    CHECK_THROWS(FilesystemManager::validate_command(real, "relative/path"));
    CHECK_THROWS(FilesystemManager::validate_command(real, ""));
    try {
        FilesystemManager::validate_command(real, "/bin/nope");
    } catch (const ExitError& e) {
        CHECK_EQ(e.code(), 127);
    } catch (...) {
        CHECK(false);
    }

    std::ofstream(root + "/bin/tool") << "x";
    FilesystemManager::validate_command(real, "/bin/tool");  // exists
    FilesystemManager::validate_command(real, "tool");       // found via PATH dirs

    std::string cmd = "rm -rf " + root;
    (void)!system(cmd.c_str());
    return finish("test_filesystem");
}
