#pragma once
// Container Manager: ties namespaces + cgroups + rootfs + process lifecycle together.
#include <cstdint>
#include <string>
#include <vector>

namespace mc {

enum class CapsMode { Default, Strict, All };

struct ContainerConfig {
    std::string name;
    std::string rootfs;
    std::string hostname;
    std::vector<std::string> command;
    std::uint64_t memory_bytes = 0;  // 0 = unlimited
    int cpu_percent = 0;             // 0 = unlimited; 50 = half of one CPU
    int pids_max = 0;                // 0 = unlimited
    bool isolate_net = false;        // optional: CLONE_NEWNET
    bool detach = false;             // optional: run in background (logs via `minicontainer logs`)
    bool no_init = false;            // workload is PID 1 itself (no reaper/forwarder)
    bool quiet = false;              // suppress the end-of-run statistics
    CapsMode caps = CapsMode::Default;
    int grace_seconds = 5;           // SIGTERM -> SIGKILL escalation
    std::string stats_file;          // optional: machine-readable key=value statistics
};

class Container {
public:
    explicit Container(ContainerConfig cfg) : cfg_(std::move(cfg)) {}
    int run();  // returns the workload's exit status

private:
    void validate();
    int run_foreground();
    int run_detached();
    ContainerConfig cfg_;
};

}  // namespace mc
