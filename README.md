<div align="center">

# MiniContainer

**A lightweight Linux container runtime in C++17, built directly on kernel namespaces, cgroups v2 and `pivot_root`.**
No Docker. No containerd. No runc. Just system calls.

![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C?logo=c%2B%2B&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux-FCC624?logo=linux&logoColor=black)
![cgroups](https://img.shields.io/badge/cgroups-v2-2ea44f)
![Build](https://img.shields.io/badge/build-CMake%203.16%2B-064F8C?logo=cmake&logoColor=white)
![Tests](https://img.shields.io/badge/acceptance%20tests-passing-brightgreen)
![License](https://img.shields.io/badge/license-MIT-blue)

</div>

---

> **Educational runtime, not a hardened sandbox.** MiniContainer exists to make container internals
> transparent and measurable. Run it in a disposable Ubuntu VM and do not use it to isolate untrusted workloads.
> See [Security model and limitations](#security-model-and-limitations).

## Table of contents

- [Overview](#overview)
- [Features](#features)
- [Quick start](#quick-start)
- [Usage](#usage)
- [Architecture](#architecture)
- [How it works](#how-it-works)
- [Testing](#testing)
- [Experiments and results](#experiments-and-results)
- [Security model and limitations](#security-model-and-limitations)
- [Project structure](#project-structure)
- [Troubleshooting](#troubleshooting)
- [Roadmap](#roadmap)
- [Contributing](#contributing)
- [License](#license)

## Overview

MiniContainer is an Operating Systems course project that re-implements the core of a container runtime from first principles.
It combines three kernel mechanisms:

| Mechanism | Kernel interface | What it provides |
|---|---|---|
| **Isolation** | `clone()` with `CLONE_NEWPID`, `NEWUTS`, `NEWNS`, `NEWIPC` (optional `NEWNET`) | Separate process tree, hostname, mount table, IPC and (optionally) network stack |
| **Resource control** | cgroups v2: `memory.max`, `cpu.max`, `pids.max` | Enforced memory, CPU and process-count limits with live statistics |
| **Filesystem isolation** | bind mount + `pivot_root` + `umount2(MNT_DETACH)` | A private root filesystem with the host filesystem unreachable |

The result is a small, readable codebase (modular C++17, no third-party dependencies) with a full CLI, a complete acceptance
test suite, and a reproducible experiment pipeline that measures container overhead and limit enforcement.

## Features

**Isolation**
- PID, UTS, mount and IPC namespaces by default; optional network namespace with loopback (`--isolate-net`)
- `pivot_root`-based root switch; old root detached, private `/proc`, `/dev` (null, zero, full, random, urandom, tty) and `/tmp`
- Mount propagation set to `rprivate`, so container mounts never leak to the host

**Resource management**
- Memory limit (`--memory 100M`) with swap disabled so the limit is enforced, not satisfied by swapping
- CPU quota (`--cpu 50` = half of one CPU) through CFS bandwidth control
- Process limit (`--pids N`) to contain fork bombs
- End-of-run statistics: peak memory, OOM kills, CPU usage, throttling

**Process lifecycle**
- Minimal built-in init as PID 1: reaps zombies and forwards `INT/TERM/HUP/QUIT/USR1/USR2` to the workload
- Graceful stop (`SIGTERM`), then forced `SIGKILL` after a configurable grace period
- Docker/shell-compatible exit codes (`128+N` for signals, `125/126/127` for runtime errors)
- Detached mode with log capture, plus `ps`, `inspect`, `stop`, `logs` and `clean`

**Reliability**
- Race-free startup: the workload cannot execute before it has joined its cgroup
- Single RAII cleanup path: no leaked cgroups, mounts or registry records on success, failure or exception
- `PR_SET_PDEATHSIG` ensures the container dies if the runner is killed
- Input validated before any side effect (name charset, rootfs, command existence inside rootfs, numeric ranges)

**Hardening (optional layer)**
- Capability bounding-set reduction (`--caps default|strict|all`) and `PR_SET_NO_NEW_PRIVS`

## Quick start

### Requirements

| Requirement | Notes |
|---|---|
| Linux, kernel 5.x+ | `memory.peak` needs 5.19+, `cgroup.kill` needs 5.14+ (both optional, with fallbacks) |
| **cgroups v2** (unified hierarchy) | Default on Ubuntu 22.04 / 24.04 |
| `memory`, `cpu`, `pids` controllers | Checked by `setup_environment.sh` |
| Root privileges | Required for namespaces and cgroup management |
| GCC 9+ or Clang 10+ (C++17), CMake 3.16+ | |
| `busybox-static` | Used to assemble the demo root filesystem |

> A dedicated Ubuntu VM is recommended. WSL and nested-container environments (for example running inside Docker) have
> different cgroup and namespace behaviour and are not supported.

### Build and run

```bash
# 1. Install dependencies and verify kernel features
sudo bash scripts/setup_environment.sh

# 2. Build the demo root filesystem (static BusyBox + test workloads) into ./rootfs
sudo bash scripts/setup_rootfs.sh

# 3. Compile
cmake -B build && cmake --build build -j

# 4. Launch an isolated, resource-limited shell
sudo ./build/minicontainer run --name demo --rootfs ./rootfs \
     --memory 100M --cpu 50 --hostname isolated -- /bin/sh
```

Inside the container, try:

```sh
hostname                  # isolated
ps                        # PID 1 = mc-init, PID 2 = /bin/sh
ls /                      # the container's own root filesystem
cat /proc/self/cgroup     # 0::/minicontainer/demo
```

Exit the shell to see the resource summary:

```text
[minicontainer] 'demo' exited with status 0 (startup 10.875 ms)
[minicontainer] memory: peak=1000.0 KiB limit=100.0 MiB oom_kill=0 limit_hits=0
[minicontainer] cpu:    usage=0.014s (user 0.002s, system 0.013s) throttled=0/18 periods (0.000s) cpu.max="50000 100000"
```

## Usage

```text
minicontainer run [options] -- <command> [args...]
minicontainer ps
minicontainer inspect <name>
minicontainer stop <name>
minicontainer logs <name>
minicontainer clean
minicontainer --help | --version
```

### Commands

| Command | Purpose |
|---|---|
| `run` | Start a container in the foreground (or `-d` in the background). Returns the workload's exit status. |
| `ps` | List running containers with PID, uptime, memory and command. |
| `inspect NAME` | Show PID, namespaces (isolated vs. shared with host), configured and live limits, and usage. |
| `stop NAME` | `SIGTERM`, then `SIGKILL` after `--grace` seconds; cleans up cgroup and registry. |
| `logs NAME` | Print the captured output of a detached container. |
| `clean` | Remove stale records, orphaned cgroups and old logs (for example after a runner was `kill -9`'d). |

### `run` options

| Option | Description | Default |
|---|---|---|
| `--rootfs DIR` | Root filesystem directory (**required**) | none |
| `--name NAME` | Container name (letters, digits, `_ - .`) | `mc-<random>` |
| `--hostname NAME` | Hostname inside the container | the name |
| `--memory SIZE` | Memory limit, e.g. `512K`, `100M`, `1G` (`memory.max`) | unlimited |
| `--cpu PERCENT` | CPU quota: `50` = half a CPU, `200` = two CPUs (`cpu.max`) | unlimited |
| `--pids N` | Maximum number of processes (`pids.max`) | unlimited |
| `--isolate-net` | Add a network namespace (loopback only) | shared with host |
| `--caps MODE` | Capability bounding set: `default`, `strict`, `all` | `default` |
| `--grace N` | Seconds between `SIGTERM` and `SIGKILL` | `5` |
| `--no-init` | Make the workload PID 1 directly (no reaper/forwarder) | init enabled |
| `-d`, `--detach` | Run in the background | foreground |
| `-q`, `--quiet` | Suppress the end-of-run summary | off |
| `--stats-file FILE` | Write machine-readable `key=value` statistics on exit | none |

### Exit codes

| Code | Meaning |
|---|---|
| `0..N` | The workload's own exit status |
| `128+N` | Workload killed by signal `N` (e.g. `137` = `SIGKILL`/OOM, `143` = `SIGTERM`) |
| `125` | Runtime error (invalid rootfs, cgroup failure, setup failure) |
| `126` | Command found but cannot be executed |
| `127` | Command not found inside the rootfs |
| `2` | Usage error |

### Examples

```bash
# Detached container, managed through the CLI
sudo ./build/minicontainer run -d --name web --rootfs ./rootfs -- /bin/sleep 600
sudo ./build/minicontainer ps
sudo ./build/minicontainer inspect web
sudo ./build/minicontainer logs web
sudo ./build/minicontainer stop web

# Memory enforcement: a 200 MiB allocation under a 50 MiB limit is OOM-killed (exit 137)
sudo ./build/minicontainer run --rootfs ./rootfs --memory 50M -- /usr/local/bin/memhog 200

# CPU enforcement: 2 CPU-seconds of work under a 50% quota takes about 4 s of wall time
sudo ./build/minicontainer run --rootfs ./rootfs --cpu 50 -- /usr/local/bin/cpuburn 2

# Fork-bomb containment
sudo ./build/minicontainer run --rootfs ./rootfs --pids 20 -- /usr/local/bin/pidtest 100

# Strongest isolation profile
sudo ./build/minicontainer run --rootfs ./rootfs --isolate-net --caps strict -- /bin/sh
```

`inspect` output for a running container:

```text
Namespaces (container vs. host):
  pid  pid:[4026533513]   isolated   (host: pid:[4026531836])
  uts  uts:[4026533511]   isolated   (host: uts:[4026531838])
  mnt  mnt:[4026533509]   isolated   (host: mnt:[4026531832])
  ipc  ipc:[4026533512]   isolated   (host: ipc:[4026531839])
  net  net:[4026531833]   shared with host

Resources:
  memory.max      100.0 MiB  (configured: 100.0 MiB)
  cpu.max         50000 100000  (configured: 50%)
```

## Architecture

```text
                 USER
                  |
             CLI (cli.cpp)  -- run | ps | inspect | stop | logs | clean
                  |
        CONTAINER MANAGER (container.cpp)  -- validation, orchestration, single cleanup path
                  |
   +--------------+--------------+------------------+
   |              |              |                  |
NamespaceMgr   ResourceMgr   FilesystemMgr      Registry (/run/minicontainer/*.state)
clone flags    cgroup v2      pivot_root, /proc,  ps / inspect / stop / logs
sethostname    memory.max     /dev, /tmp
loopback up    cpu.max
               pids.max
   +--------------+--------------+
                  |
          PROCESS MANAGER (process_manager.cpp)
          clone() + sync pipe + error pipe + signal forwarding
                  |
     runner (host)  --clone-->  container init (PID 1)  --fork-->  workload (PID 2)
```

| Module | Location | Responsibility |
|---|---|---|
| CLI | `src/cli` | Argument parsing, subcommands, user-facing errors and exit codes |
| Container Manager | `src/container` | Validate, create cgroup, clone, attach, release, wait, collect stats, clean up |
| Namespace Manager | `src/namespace` | `clone()` flags, hostname, loopback, namespace inspection |
| Resource Manager | `src/resource` | cgroup v2 hierarchy, limits, statistics, removal |
| Filesystem Manager | `src/filesystem` | Rootfs validation, `pivot_root`, `/proc`, `/dev`, `/tmp` |
| Process Manager | `src/process` | Child creation, parent/child sync, PID-1 init, signals, exit status |
| Common | `src/common` | `SysError`, size parsing, file I/O, on-disk registry |

## How it works

### Startup sequence

```text
runner (host)                              container init (new namespaces)
-------------                              -------------------------------
validate config, rootfs, command, limits
prepare cgroup hierarchy; mkdir <name>;
  write memory.max / cpu.max / pids.max
clone(NEWPID|NEWUTS|NEWNS|NEWIPC) -------> blocks reading the sync pipe
write child PID to cgroup.procs
save registry record
write 'G' to sync pipe ------------------> wakes up
                                           sethostname; make / rprivate; bind rootfs;
                                           pivot_root; umount2(old root, MNT_DETACH);
                                           mount /proc, /dev, /tmp
                                           fork() -> workload: drop caps, execvpe(command)
read error pipe (EOF == exec ok) <-------- (CLOEXEC pipe closes on exec; failures written to it)
waitpid(init), forwarding signals          init: waitpid(-1) loop, forwards signals,
                                                 exits with the workload's status
read cgroup stats -> print summary
rm cgroup, rm registry record
```

### Key design decisions

1. **Race-free cgroup attachment.** The cloned child blocks on a pipe. The parent writes the child's host PID into `cgroup.procs` and only then releases it, so the workload never runs outside its limits.
2. **The PID 1 problem.** In a PID namespace the first process ignores signals that have no handler and must reap orphans. By default the runtime places a tiny init at PID 1 that forwards signals and reaps every child; `--no-init` runs the workload as PID 1 directly.
3. **Exec failure detection.** A `CLOEXEC` error pipe closes on a successful `exec`. Any failure is written to it, letting the runtime distinguish *not found* (127), *cannot execute* (126) and *runtime error* (125).
4. **Graceful, then forced termination.** The first `SIGTERM`/`SIGHUP`/`SIGQUIT` is forwarded as `SIGTERM` with an alarm. When the grace period expires, or on a second signal, init is `SIGKILL`ed and the kernel tears down the whole PID namespace. Three `Ctrl+C` presses force an immediate kill.
5. **No orphans, no leaks.** `PR_SET_PDEATHSIG(SIGKILL)` covers a dead runner, a single RAII cleanup handles every error path (cgroup removal retries on `EBUSY`), and `minicontainer clean` repairs the `kill -9` case.
6. **Strict limits.** `memory.swap.max=0` makes the memory limit a hard limit.
7. **Validation before side effects.** Names are restricted to a safe charset (no path traversal), the rootfs must exist and must not be `/`, and the command must exist *inside* the rootfs.
8. **Statistics from cgroup files**, read after the workload exits and before the cgroup is removed.

### Kernel interfaces used

| Need | Interface |
|---|---|
| Isolated child | `clone(CLONE_NEW* \| SIGCHLD)` with an `mmap`ed stack |
| Parent/child sync | `pipe2(O_CLOEXEC)` for the go-pipe and error-pipe |
| New root | `mount(MS_BIND)`, `syscall(SYS_pivot_root)`, `umount2(MNT_DETACH)` |
| Devices | tmpfs + `mknod` for `null`, `zero`, `full`, `random`, `urandom`, `tty` |
| Resources | `/sys/fs/cgroup/minicontainer/<name>/{memory.max, memory.swap.max, cpu.max, pids.max, cgroup.procs, cgroup.kill}` |
| Statistics | `memory.current`, `memory.peak`, `memory.events`, `cpu.stat`, `pids.current` |
| Lifecycle | `waitpid`, `kill`, `sigaction`, `alarm`, `prctl(PR_SET_PDEATHSIG)` |
| Hardening | `prctl(PR_CAPBSET_DROP)`, `PR_SET_NO_NEW_PRIVS` |

## Testing

```bash
ctest --test-dir build --output-on-failure   # unit tests (end-to-end ones run when root + rootfs are available)
sudo bash scripts/run_tests.sh               # full acceptance suite
```

The acceptance suite (`scripts/run_tests.sh`) validates the following areas end to end. At the time of writing, every check passes.

| Area | What is verified |
|---|---|
| Launch and exit status | Output capture, exit code propagation, `128+N` for signals |
| PID namespace | Workload/init PIDs, reduced process view, differing namespace inode |
| UTS namespace | `--hostname` applied, in-container change does not touch the host |
| Mount namespace | Container mounts invisible to the host; rootfs matches the directory |
| Filesystem isolation | Host files unreachable, old root detached, private `/proc` |
| Security hardening | Reduced capability set, `--caps strict`, `--isolate-net` |
| Memory limit | OOM kill at the limit, `memory.events`, `memory.peak` within limit |
| CPU limit | Unrestricted ratio near 1.0, 50% quota near 0.5, throttling observed |
| PID limit | Fork storm halted by `pids.max` |
| Independence | Changing one container's limits leaves another unchanged |
| Lifecycle | `SIGTERM`/`SIGINT` forwarding, forced kill after grace, `PDEATHSIG` |
| Failure paths | Bad rootfs, missing command, bad size, path traversal leave nothing behind |
| Stress | 25 sequential containers with varied limits |
| Cleanup | No stale cgroups, processes or registry records |
| CLI | `--help`, unknown command and missing `--rootfs` handling |

Unit tests use a small dependency-free framework (`tests/test_framework.hpp`) and cover namespaces, resource parsing and
cgroup creation, filesystem validation, and the registry and CLI.

## Experiments and results

The experiment pipeline reproduces the project's measurements and renders graphs for the report:

```bash
sudo REPS=5 bash scripts/run_experiments.sh   # writes results/*.csv and *.txt
python3 scripts/plot_results.py               # writes results/*.png (needs python3-matplotlib)
```

| # | Experiment | Output |
|---|---|---|
| 1 | Native vs. containerized cost (CPU job, startup latency, memory overhead) | `exp1_cpu.csv`, `exp1_startup.csv`, `exp1_memory_overhead.csv` |
| 2 | CPU quota sweep (100 / 75 / 50 / 25 %) | `exp2_cpu_quota.csv` |
| 3 | Memory limit sweep (none / 200M / 100M / 50M) with a 150 MiB workload | `exp3_memory.csv` |
| 4 | Namespace views (inode numbers, hostname, process and mount counts) | `exp4_namespaces.txt` |
| 5 | Independent limits across concurrent containers | `exp5_multi.txt` |

### Representative measurements

Collected on Ubuntu 24.04 (GCC 13.3, BusyBox 1.36.1, cgroups v2).

| Scenario | Result |
|---|---|
| Container startup | about 8 to 11 ms for a shell |
| Idle shell memory footprint | about 1 MiB peak |
| 2 CPU-second job, **50% quota** | wall = 3.99 s, cpu/wall ratio = **0.501**, throttled in 40 of 41 periods |
| 200 MiB allocation, **50 MiB limit** | killed with exit **137**, `oom_kill=1`, peak = 50.0 MiB |
| 100 process fork attempt, `--pids 20` | halted at 18 forks (`Resource temporarily unavailable`) |
| Namespace comparison | host: 346 processes / 51 mounts, container: 5 processes / 5 mounts |

CPU enforcement follows CFS bandwidth control: a quota of `50000 / 100000` lets the group run 50 ms in every 100 ms period,
so a 2 CPU-second job needs roughly 4 seconds of wall time.

## Security model and limitations

MiniContainer is deliberately simple and is **not** a security boundary against hostile code.

- **Root is required.** There is no user-namespace (rootless) mode.
- **`--caps default` keeps `CAP_SYS_ADMIN`** so that hostname and mount demonstrations work. With it, a container could mount cgroupfs and modify its own limits. Use `--caps strict` for a stronger profile.
- **No seccomp filtering**, no read-only or masked `/proc` paths, and no cgroup namespace.
- **Networking is minimal:** the host network is shared by default; `--isolate-net` provides loopback only (no veth, bridge or NAT).
- **No image format or layered filesystem:** the rootfs is a plain directory.
- Kernel vulnerabilities bypass namespaces and cgroups entirely.

Run it in a disposable VM, and never with untrusted workloads.

## Project structure

```text
.
├── CMakeLists.txt
├── README.md
├── LICENSE
├── docs/                     # architecture, implementation notes, experiments
├── include/                  # public headers, one directory per module
│   ├── cli/  common/  container/  filesystem/  namespace/  process/  resource/
├── src/
│   ├── main.cpp
│   ├── cli/                  # argument parsing and subcommands
│   ├── common/               # util + on-disk registry
│   ├── container/            # orchestration and cleanup
│   ├── filesystem/           # rootfs validation and pivot_root
│   ├── namespace/            # clone flags, hostname, loopback
│   ├── process/              # clone, init, signals
│   └── resource/             # cgroup v2 management
├── tests/                    # unit and integration tests
├── scripts/
│   ├── setup_environment.sh  # dependencies + kernel feature check
│   ├── setup_rootfs.sh       # BusyBox rootfs + test workloads
│   ├── run_tests.sh          # acceptance suite
│   ├── run_experiments.sh    # experiments 1-5
│   └── plot_results.py       # CSV -> PNG graphs
└── tools/                    # static test workloads: memhog, cpuburn, pidtest
```

## Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `minicontainer needs root privileges` | Run with `sudo`. |
| `cgroups v2 is not mounted` | Run `stat -fc %T /sys/fs/cgroup`. It must print `cgroup2fs`. On older releases add `systemd.unified_cgroup_hierarchy=1` to the kernel command line. |
| `could not enable the 'memory'/'cpu' controller` | Running inside a nested cgroup namespace (e.g. Docker) or a cgroup that already has processes. Use a dedicated VM. |
| `command '...' not found inside rootfs` (exit 127) | The path must exist *inside* the rootfs. Re-run `scripts/setup_rootfs.sh`. |
| `memory.peak` shows `n/a` | Kernel older than 5.19. The feature is optional. |
| Leftover cgroups or records after a crash | Run `sudo ./build/minicontainer clean`. |
| `plot_results.py` fails to import matplotlib | `sudo apt install python3-matplotlib`, or use a virtual environment. |

## Roadmap

- [ ] seccomp-BPF syscall filtering
- [ ] User namespaces and rootless mode
- [ ] Network namespace with veth pair, bridge and NAT
- [ ] Read-only and masked `/proc` and `/sys` paths
- [ ] cgroup namespace
- [ ] Overlay filesystem and image layers
- [ ] I/O limits (`io.max`)

## Contributing

Issues and pull requests are welcome.

1. Fork the repository and create a feature branch.
2. Build with `-Wall -Wextra -Wpedantic` (already set in `CMakeLists.txt`) and keep the build warning-free.
3. Add or update tests, then run `ctest --test-dir build` and `sudo bash scripts/run_tests.sh`.
4. Describe the change and its kernel-interface implications in the pull request.

## License

Released under the [MIT License](LICENSE). Copyright (c) 2026 Ashutosh Kumar Jha.

## Acknowledgements

Built as an Operating Systems course project. Inspired by the Linux kernel documentation for
[namespaces(7)](https://man7.org/linux/man-pages/man7/namespaces.7.html),
[cgroups(7)](https://man7.org/linux/man-pages/man7/cgroups.7.html) and
[pivot_root(2)](https://man7.org/linux/man-pages/man2/pivot_root.2.html), and by the design of OCI runtimes such as `runc`.
