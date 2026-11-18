# Implementation notes (for the report / viva)

## System calls and kernel interfaces used
| Need | Interface | Where |
|---|---|---|
| Isolated child | `clone(CLONE_NEWPID\|NEWUTS\|NEWNS\|NEWIPC[\|NEWNET] \| SIGCHLD)` with an `mmap`ed 1 MiB stack (high end passed; stack grows down) | `ProcessManager::spawn` |
| Parent/child sync | `pipe2(O_CLOEXEC)` x2 (go-pipe, error-pipe) | `ProcessManager` |
| Hostname | `sethostname` | `NamespaceManager` |
| Private mounts | `mount(NULL,"/",NULL,MS_REC\|MS_PRIVATE)` | `FilesystemManager::enter_root` |
| New root | bind mount rootfs onto itself, `pivot_root(".", ".pivot_old")` via `syscall(SYS_pivot_root)`, `umount2(MNT_DETACH)` | same |
| /proc, /dev, /tmp | `mount("proc")`, tmpfs + `mknod` for null/zero/full/random/urandom/tty | same |
| Resources | files under `/sys/fs/cgroup/minicontainer/<name>/`: `memory.max`, `memory.swap.max`, `cpu.max`, `pids.max`, `cgroup.procs`, `cgroup.kill`; stats from `memory.current/peak/events`, `cpu.stat`, `pids.current` | `ResourceManager` |
| Exec | `fork` + `execvpe` | `ProcessManager::exec_workload` |
| Lifecycle | `waitpid`, `kill`, `sigaction`, `alarm`, `prctl(PR_SET_PDEATHSIG)` | `ProcessManager` |
| Hardening | `prctl(PR_CAPBSET_DROP)`, `PR_SET_NO_NEW_PRIVS` | `container.cpp` |

## Key design decisions
1. **Race-free cgroup attach.** The child blocks on a pipe; the parent writes the child's *host* PID to `cgroup.procs`, only then releases it.
2. **PID 1 problem.** In a PID namespace the first process is PID 1: the kernel ignores signals without handlers for it, and it must reap orphans.
   By default the cloned child is a minimal init: it installs handlers that forward `INT/TERM/HUP/QUIT/USR1/USR2` to the workload, reaps *every* child
   with `waitpid(-1)`, and exits with the workload's status (128+N if signalled). `--no-init` makes the workload PID 1 directly (like `docker run` without `--init`).
3. **Exec failure detection.** The error pipe is `O_CLOEXEC`: if `execvpe` succeeds the pipe just closes (EOF); if anything fails the child writes `"<code>\n<message>"`.
   The runtime maps this to 127 (not found), 126 (cannot execute), 125 (runtime error).
4. **Graceful then forced termination.** First SIGTERM/HUP/QUIT -> forward SIGTERM + `alarm(grace)`; SIGALRM or a second signal -> SIGKILL of init (kernel then kills the whole namespace);
   three Ctrl+C -> SIGKILL. `stop NAME` signals the runner so cleanup still happens in one place.
5. **No orphans, no leaks.** `PR_SET_PDEATHSIG(SIGKILL)` kills the container if the runner dies; all setup is guarded by one RAII cleanup (kill, remove cgroup with retry on `EBUSY`, delete registry record);
   `minicontainer clean` repairs the case where the runner itself was `kill -9`'d.
6. **Strict limits.** `memory.swap.max=0` so the memory limit is enforced rather than satisfied by swapping.
7. **Validation before side effects.** Name charset (no path traversal), rootfs exists and is not `/`, command exists *inside* the rootfs, numeric ranges, controllers available and delegated.
8. **Statistics from cgroup files**, read after the workload exits and before the cgroup is removed (`--stats-file` exports them for the experiments).

## Optional features implemented
Network namespace (`--isolate-net`, loopback brought up), PID limit, detached execution, `ps`, `inspect`, `stop`, `logs`, capability dropping (`--caps`), container log capture.
Not implemented: seccomp filtering (see Future Scope).

## Limitations (include in the report)
* Root is required; there is no user namespace/rootless mode.
* `--caps default` keeps `CAP_SYS_ADMIN` so that the PRD's hostname/mount tests work. With it a container could mount cgroupfs and edit its own limits; use `--caps strict` for stronger isolation.
* No seccomp, no read-only/masked `/proc` paths, no cgroup namespace, no image format, no networking beyond loopback.
* Not safe for untrusted workloads; kernel vulnerabilities bypass namespaces and cgroups.
