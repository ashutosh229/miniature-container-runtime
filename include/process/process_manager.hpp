#pragma once
// Process Manager: clone() the container init, synchronise with it, supervise it.
#include <sys/types.h>

#include <functional>
#include <string>
#include <vector>

namespace mc {

struct ChildContext {
    std::function<void()> prepare;   // runs in container init (PID 1) after release: hostname, rootfs, ...
    std::function<void()> pre_exec;  // runs in the workload process right before exec: capabilities, ...
    std::vector<std::string> argv;
    std::vector<std::string> envp;
    bool use_init = true;  // false: the workload itself becomes PID 1 (like `docker run` without --init)
};

class ProcessManager {
public:
    ProcessManager() = default;
    ~ProcessManager();
    ProcessManager(const ProcessManager&) = delete;
    ProcessManager& operator=(const ProcessManager&) = delete;

    // Optional: lets the container's init rename itself ("mc-init") in /proc/1/cmdline.
    static void capture_argv(int argc, char** argv);
    void set_grace_period(int seconds) { grace_ = seconds; }
    // clone() the child with the given CLONE_NEW* flags. The child blocks until release().
    pid_t spawn(int clone_flags, ChildContext ctx);
    void release();  // child may now set up its namespaces and exec
    void cancel();   // child must abort (parent failed to configure the cgroup)
    // Blocks until the workload has exec'd (returns "") or setup failed ("<code>\n<message>").
    std::string read_child_error();
    // Waits for the container, forwarding SIGINT/SIGTERM/SIGHUP/SIGQUIT and escalating to SIGKILL
    // after the grace period. Returns the exit status (128+signal if killed by a signal).
    int wait();
    void kill_and_reap() noexcept;
    pid_t pid() const { return pid_; }

private:
    static int child_entry(void* arg);
    int child_main();
    void report(int code, const std::string& msg) const;
    [[noreturn]] void exec_workload() const;

    int sync_pipe_[2] = {-1, -1};  // parent -> child: 'G' = go
    int err_pipe_[2] = {-1, -1};   // child -> parent: CLOEXEC, so EOF means exec succeeded
    pid_t pid_ = -1;
    bool reaped_ = false;
    int grace_ = 5;
    ChildContext ctx_;
};

}  // namespace mc
