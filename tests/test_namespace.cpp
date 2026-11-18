#include <sched.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/util.hpp"
#include "container/container.hpp"
#include "namespace/namespace_manager.hpp"
#include "test_framework.hpp"

using namespace mc;

int main() {
    ContainerConfig c;
    int f = NamespaceManager::clone_flags(c);
    CHECK(f & CLONE_NEWPID);
    CHECK(f & CLONE_NEWUTS);
    CHECK(f & CLONE_NEWNS);
    CHECK(f & CLONE_NEWIPC);
    CHECK(!(f & CLONE_NEWNET));
    c.isolate_net = true;
    CHECK(NamespaceManager::clone_flags(c) & CLONE_NEWNET);

    // Describing ourselves: every namespace is, by definition, shared with the "host" (us).
    auto info = NamespaceManager::describe(getpid());
    CHECK_EQ(info.size(), static_cast<std::size_t>(5));
    for (const auto& n : info) {
        CHECK(!n.isolated);
        CHECK_EQ(n.id, n.host_id);
    }

    if (geteuid() == 0) {
        // Prove a real UTS namespace isolates the hostname, without touching the host's.
        char before[256], after[256];
        gethostname(before, sizeof before);
        pid_t p = fork();
        if (p == 0) {
            if (unshare(CLONE_NEWUTS) != 0) _exit(2);
            if (sethostname("mc-ns-test", 10) != 0) _exit(3);
            _exit(0);
        }
        int st = 0;
        waitpid(p, &st, 0);
        gethostname(after, sizeof after);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        CHECK_EQ(std::string(before), std::string(after));
    } else {
        SKIP("UTS namespace test needs root");
    }
    return finish("test_namespace");
}
