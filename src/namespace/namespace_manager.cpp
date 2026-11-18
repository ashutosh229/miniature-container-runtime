#include "namespace/namespace_manager.hpp"

#include <net/if.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "common/util.hpp"
#include "container/container.hpp"

namespace mc {

int NamespaceManager::clone_flags(const ContainerConfig& cfg) {
    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWIPC;
    if (cfg.isolate_net) flags |= CLONE_NEWNET;
    return flags;
}

void NamespaceManager::set_hostname(const std::string& hostname) {
    if (::sethostname(hostname.c_str(), hostname.size()) != 0) throw SysError("sethostname");
}

void NamespaceManager::bring_up_loopback() {
    int fd = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) throw SysError("socket");
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof ifr);
    std::strncpy(ifr.ifr_name, "lo", IFNAMSIZ - 1);
    if (::ioctl(fd, SIOCGIFFLAGS, &ifr) != 0) {
        int e = errno;
        ::close(fd);
        throw SysError("SIOCGIFFLAGS lo", e);
    }
    ifr.ifr_flags |= IFF_UP;
    if (::ioctl(fd, SIOCSIFFLAGS, &ifr) != 0) {
        int e = errno;
        ::close(fd);
        throw SysError("SIOCSIFFLAGS lo", e);
    }
    ::close(fd);
}

namespace {
std::string ns_link(const std::string& proc_dir, const char* name) {
    char buf[128];
    ssize_t n = ::readlink((proc_dir + "/ns/" + name).c_str(), buf, sizeof buf - 1);
    return n > 0 ? std::string(buf, static_cast<std::size_t>(n)) : std::string("?");
}
}  // namespace

std::vector<NsInfo> NamespaceManager::describe(pid_t pid) {
    std::vector<NsInfo> out;
    for (const char* n : {"pid", "uts", "mnt", "ipc", "net"}) {
        NsInfo i;
        i.name = n;
        i.id = ns_link("/proc/" + std::to_string(pid), n);
        i.host_id = ns_link("/proc/self", n);
        i.isolated = i.id != "?" && i.id != i.host_id;
        out.push_back(i);
    }
    return out;
}

}  // namespace mc
