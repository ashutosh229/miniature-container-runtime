// Registry unit tests (always run) + end-to-end tests that drive the real binary (root + rootfs only).
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "common/registry.hpp"
#include "common/util.hpp"
#include "test_framework.hpp"

using namespace mc;

struct Result { std::string out; int code; };

static Result sh(const std::string& cmd) {
    Result r{"", -1};
    FILE* p = popen((cmd + " 2>&1").c_str(), "r");
    if (!p) return r;
    char buf[512];
    while (std::fgets(buf, sizeof buf, p)) r.out += buf;
    int st = pclose(p);
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return r;
}

int main() {
    // ---------- registry round trip ----------
    char tmpl[] = "/tmp/mc-state-XXXXXX";
    std::string dir = mkdtemp(tmpl);
    setenv("MC_STATE_DIR", dir.c_str(), 1);
    ContainerState s;
    s.name = "t1";
    s.pid = 4242;
    s.runner_pid = getpid();
    s.command = "/bin/sh -c echo a=b";
    s.memory_bytes = 104857600;
    s.cpu_percent = 50;
    s.detached = true;
    Registry::save(s);
    auto loaded = Registry::load("t1");
    CHECK(loaded.has_value());
    if (loaded) {
        CHECK_EQ(loaded->pid, 4242);
        CHECK_EQ(loaded->command, std::string("/bin/sh -c echo a=b"));
        CHECK_EQ(loaded->memory_bytes, 104857600ULL);
        CHECK(loaded->detached);
        CHECK(Registry::is_running(*loaded));  // runner_pid == us
    }
    CHECK_EQ(Registry::list().size(), static_cast<std::size_t>(1));
    CHECK(!Registry::load("../evil").has_value());
    Registry::remove("t1");
    CHECK(!Registry::load("t1").has_value());
    CHECK(!Registry::process_alive(0));
    (void)!system(("rm -rf " + dir).c_str());
    unsetenv("MC_STATE_DIR");

    // ---------- CLI behaviour that needs no root ----------
    std::string bin = MC_BINARY;
    CHECK_EQ(sh(bin + " --help").code, 0);
    CHECK_EQ(sh(bin + " --version").code, 0);
    CHECK_EQ(sh(bin + " run").code, 2);                                     // usage error
    CHECK_EQ(sh(bin + " run --bogus -- /bin/true").code, 2);
    CHECK_EQ(sh(bin + " run --rootfs /x --memory 12Q -- /bin/true").code, 2);
    CHECK_EQ(sh(bin + " frobnicate").code, 2);

    // ---------- end-to-end (root + prepared rootfs) ----------
    const char* env_rootfs = std::getenv("MC_TEST_ROOTFS");
    std::string rootfs = env_rootfs ? env_rootfs : "./rootfs";
    if (geteuid() != 0 || access((rootfs + "/bin/sh").c_str(), X_OK) != 0) {
        SKIP("end-to-end tests need root and a rootfs (scripts/setup_rootfs.sh; set MC_TEST_ROOTFS)");
        return finish("test_container");
    }
    std::string run = bin + " run -q --rootfs " + rootfs;
    auto r = sh(run + " -- /bin/echo hello");
    CHECK_EQ(r.code, 0);
    CHECK_EQ(r.out, std::string("hello\n"));
    CHECK_EQ(sh(run + " -- /bin/sh -c 'exit 7'").code, 7);
    CHECK_EQ(sh(run + " --no-init -- /bin/sh -c 'echo $$'").out, std::string("1\n"));  // workload is PID 1
    CHECK_EQ(sh(run + " -- /bin/sh -c 'echo $$'").out, std::string("2\n"));            // init is PID 1, shell is PID 2
    CHECK_EQ(sh(run + " --hostname isolated -- /bin/hostname").out, std::string("isolated\n"));
    CHECK_EQ(sh(run + " -- /bin/nonexistent").code, 127);
    CHECK_EQ(sh(bin + " run -q --rootfs /does/not/exist -- /bin/true").code, 125);
    CHECK_EQ(sh(run + " -- /bin/sh -c 'kill -9 $$'").code, 137);
    return finish("test_container");
}
