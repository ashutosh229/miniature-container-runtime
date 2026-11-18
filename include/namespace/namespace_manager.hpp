#pragma once
// Namespace Manager: picks clone() flags and configures UTS / network inside the child.
#include <sys/types.h>

#include <string>
#include <vector>

namespace mc {

struct ContainerConfig;

struct NsInfo {
    std::string name;     // pid, uts, mnt, ipc, net
    std::string id;       // e.g. "pid:[4026532201]"
    std::string host_id;  // the same namespace as seen by the calling (host) process
    bool isolated = false;
};

class NamespaceManager {
public:
    // CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC  (+ CLONE_NEWNET if requested)
    static int clone_flags(const ContainerConfig& cfg);
    static void set_hostname(const std::string& hostname);  // child side (UTS namespace)
    static void bring_up_loopback();                         // child side (net namespace)
    static std::vector<NsInfo> describe(pid_t pid);          // compares /proc/<pid>/ns/* with ours
};

}  // namespace mc
