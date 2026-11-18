#pragma once
// Tiny on-disk registry (/run/minicontainer/<name>.state) so that `ps`, `inspect`,
// `stop` and `logs` can find containers started by another minicontainer process.
#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mc {

struct ContainerState {
    std::string name;
    pid_t pid = 0;         // container init (PID 1 in its namespace), host PID
    pid_t runner_pid = 0;  // the minicontainer process supervising it
    std::string cgroup;
    std::string rootfs;
    std::string hostname;
    std::string command;
    std::uint64_t memory_bytes = 0;
    int cpu_percent = 0;
    int pids_max = 0;
    int grace = 5;
    bool isolate_net = false;
    bool detached = false;
    std::int64_t started = 0;  // unix seconds
};

class Registry {
public:
    static std::string dir();  // $MC_STATE_DIR or /run/minicontainer
    static std::string state_path(const std::string& name);
    static std::string log_path(const std::string& name);
    static void save(const ContainerState& st);
    static std::optional<ContainerState> load(const std::string& name);
    static std::vector<ContainerState> list();
    static void remove(const std::string& name) noexcept;
    static bool process_alive(pid_t pid);
    static bool is_running(const ContainerState& st);
};

}  // namespace mc
