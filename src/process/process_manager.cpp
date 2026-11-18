#include "process/process_manager.hpp"

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include "common/util.hpp"

namespace mc {
namespace {

constexpr std::size_t kStackSize = 1024 * 1024;
const int kForwardSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2};

// ------------------------- runtime (host) side -------------------------
volatile sig_atomic_t g_target = 0;  // host PID of container init; 0 = nothing to forward to
volatile sig_atomic_t g_int_count = 0;
volatile sig_atomic_t g_term_count = 0;
volatile sig_atomic_t g_grace = 5;

const int kRuntimeSignals[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGALRM};
struct sigaction g_old[5];
bool g_installed = false;

void runtime_handler(int sig) {
    const int saved = errno;
    const pid_t t = g_target;
    if (t > 0) {
        if (sig == SIGALRM) {
            kill(t, SIGKILL);  // grace period expired
        } else if (sig == SIGINT) {
            if (++g_int_count >= 3) kill(t, SIGKILL);  // Ctrl+C x3 = force
            else kill(t, SIGINT);
        } else {  // SIGTERM / SIGHUP / SIGQUIT: graceful stop, then SIGKILL
            if (g_term_count++ == 0 && g_grace > 0) {
                kill(t, SIGTERM);
                alarm(static_cast<unsigned>(g_grace));
            } else {
                kill(t, SIGKILL);
            }
        }
    }
    errno = saved;
}

void install_runtime_handlers() {
    if (g_installed) return;
    for (std::size_t i = 0; i < 5; ++i) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_handler = runtime_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(kRuntimeSignals[i], &sa, &g_old[i]);
    }
    g_installed = true;
}

void restore_runtime_handlers() {
    if (!g_installed) return;
    for (std::size_t i = 0; i < 5; ++i) sigaction(kRuntimeSignals[i], &g_old[i], nullptr);
    g_installed = false;
}

// ------------------------- container init (PID 1) side -------------------------
volatile sig_atomic_t g_workload = 0;

void init_handler(int sig) {
    const int saved = errno;
    const pid_t w = g_workload;
    if (w > 0) kill(w, sig);  // PID 1 has no default signal actions, so it must forward explicitly
    errno = saved;
}

char* g_argv_start = nullptr;
std::size_t g_argv_len = 0;

void write_all(int fd, const std::string& s) {
    std::size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::write(fd, s.data() + off, s.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return;
        }
        off += static_cast<std::size_t>(n);
    }
}

}  // namespace

void ProcessManager::capture_argv(int argc, char** argv) {
    if (argc < 1) return;
    g_argv_start = argv[0];
    g_argv_len = static_cast<std::size_t>(argv[argc - 1] + std::strlen(argv[argc - 1]) + 1 - argv[0]);
}

ProcessManager::~ProcessManager() {
    kill_and_reap();
    for (int* fd : {&sync_pipe_[0], &sync_pipe_[1], &err_pipe_[0], &err_pipe_[1]})
        if (*fd >= 0) {
            ::close(*fd);
            *fd = -1;
        }
    g_target = 0;
    restore_runtime_handlers();
}

pid_t ProcessManager::spawn(int clone_flags, ChildContext ctx) {
    ctx_ = std::move(ctx);
    g_grace = grace_;
    g_int_count = g_term_count = 0;
    if (::pipe2(sync_pipe_, O_CLOEXEC) != 0) throw SysError("pipe2");
    if (::pipe2(err_pipe_, O_CLOEXEC) != 0) throw SysError("pipe2");
    install_runtime_handlers();  // before clone: Ctrl+C during setup must not skip cleanup

    // clone() does not allocate the child's stack: we do. The stack grows down on x86-64/arm64,
    // so we pass the HIGH end. Without CLONE_VM the child gets a private copy, so we may unmap ours.
    void* stack = ::mmap(nullptr, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_STACK, -1, 0);
    if (stack == MAP_FAILED) throw SysError("mmap child stack");
    char* top = static_cast<char*>(stack) + kStackSize;
    pid_ = ::clone(&ProcessManager::child_entry, top, clone_flags | SIGCHLD, this);
    const int e = errno;
    ::munmap(stack, kStackSize);
    if (pid_ < 0) throw SysError("clone (needs root and namespace support)", e);

    ::close(sync_pipe_[0]);
    sync_pipe_[0] = -1;
    ::close(err_pipe_[1]);
    err_pipe_[1] = -1;
    g_target = pid_;
    return pid_;
}

void ProcessManager::release() {
    if (sync_pipe_[1] < 0) return;
    const char go = 'G';
    ssize_t n;
    do { n = ::write(sync_pipe_[1], &go, 1); } while (n < 0 && errno == EINTR);
    ::close(sync_pipe_[1]);
    sync_pipe_[1] = -1;
    if (n != 1) throw SysError("release child");
}

void ProcessManager::cancel() {
    if (sync_pipe_[1] >= 0) {
        ::close(sync_pipe_[1]);  // EOF without 'G' -> child exits with 125
        sync_pipe_[1] = -1;
    }
}

std::string ProcessManager::read_child_error() {
    std::string out;
    char buf[512];
    for (;;) {
        ssize_t n = ::read(err_pipe_[0], buf, sizeof buf);
        if (n > 0) out.append(buf, static_cast<std::size_t>(n));
        else if (n == 0) break;
        else if (errno == EINTR) continue;
        else break;
    }
    return out;
}

int ProcessManager::wait() {
    int st = 0;
    for (;;) {
        pid_t r = ::waitpid(pid_, &st, 0);
        if (r == pid_) break;
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) throw SysError("waitpid");
    }
    reaped_ = true;
    g_target = 0;
    ::alarm(0);
    restore_runtime_handlers();
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

void ProcessManager::kill_and_reap() noexcept {
    if (pid_ > 0 && !reaped_) {
        ::kill(pid_, SIGKILL);
        int st;
        while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
        reaped_ = true;
    }
}

// ---------------------------------------------------------------------------
// Everything below runs in the cloned child (PID 1 of the new PID namespace).
// ---------------------------------------------------------------------------
int ProcessManager::child_entry(void* arg) { return static_cast<ProcessManager*>(arg)->child_main(); }

void ProcessManager::report(int code, const std::string& msg) const {
    write_all(err_pipe_[1], std::to_string(code) + "\n" + msg);
}

void ProcessManager::exec_workload() const {
    for (int sig : kForwardSignals) ::signal(sig, SIG_DFL);
    ::signal(SIGPIPE, SIG_DFL);  // the runtime ignores SIGPIPE; the workload must not inherit that
    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, nullptr);
    try {
        if (ctx_.pre_exec) ctx_.pre_exec();
    } catch (const std::exception& ex) {
        report(126, std::string("pre-exec setup failed: ") + ex.what());
        ::_exit(126);
    }
    std::vector<char*> av, ev;
    for (auto& s : ctx_.argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(nullptr);
    for (auto& s : ctx_.envp) {
        ev.push_back(const_cast<char*>(s.c_str()));
        if (s.rfind("PATH=", 0) == 0) ::setenv("PATH", s.c_str() + 5, 1);  // execvpe searches *our* PATH
    }
    ev.push_back(nullptr);
    ::execvpe(av[0], av.data(), ev.data());
    const int e = errno;
    report(e == ENOENT ? 127 : 126, "exec '" + ctx_.argv[0] + "': " + std::strerror(e));
    ::_exit(e == ENOENT ? 127 : 126);
}

int ProcessManager::child_main() {
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);  // if the runtime dies, the container dies with it (no orphans)
    ::close(sync_pipe_[1]);
    ::close(err_pipe_[0]);

    // We are PID 1: install forwarding handlers first (the inherited runtime handlers must not run here).
    for (int sig : kForwardSignals) {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_handler = init_handler;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
    }
    struct sigaction dfl;
    std::memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(SIGALRM, &dfl, nullptr);

    // Block until the parent has put us into the cgroup.
    char go = 0;
    ssize_t n;
    do { n = ::read(sync_pipe_[0], &go, 1); } while (n < 0 && errno == EINTR);
    ::close(sync_pipe_[0]);
    if (n != 1 || go != 'G') ::_exit(125);

    try {
        if (ctx_.prepare) ctx_.prepare();
    } catch (const std::exception& ex) {
        report(125, std::string("container setup failed: ") + ex.what());
        ::_exit(125);
    }

    if (!ctx_.use_init) {
        // No init/reaper: the workload execs directly and IS PID 1 (it must handle signals / reap children itself).
        exec_workload();
    }

    sigset_t fwd, old;
    sigemptyset(&fwd);
    for (int sig : kForwardSignals) sigaddset(&fwd, sig);
    sigprocmask(SIG_BLOCK, &fwd, &old);

    if (g_argv_start && g_argv_len > 8) {  // make `ps` inside the container show a sensible PID 1
        std::memset(g_argv_start, 0, g_argv_len);
        std::strcpy(g_argv_start, "mc-init");
    }

    pid_t w = ::fork();
    if (w < 0) {
        report(125, std::string("fork: ") + std::strerror(errno));
        ::_exit(125);
    }
    if (w == 0) exec_workload();

    // ---- PID 1: minimal init. Close our copy of the error pipe so the parent sees EOF at exec. ----
    ::close(err_pipe_[1]);
    g_workload = w;
    sigprocmask(SIG_SETMASK, &old, nullptr);
    int code = 125;
    for (;;) {
        int st = 0;
        pid_t r = ::waitpid(-1, &st, 0);  // reap EVERY child, including re-parented orphans
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == w) {
            code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            break;
        }
    }
    ::_exit(code);  // the kernel SIGKILLs every other process still in this PID namespace
}

}  // namespace mc
