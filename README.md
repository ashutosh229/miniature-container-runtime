# MiniContainer

A lightweight Linux container runtime in C++17 that uses **namespaces** (PID, UTS, mount, IPC, optional net),
**cgroups v2** (memory, CPU, PIDs) and **pivot_root** directly - no Docker, no containerd.
Built as an Operating Systems course project (see `docs/` for the architecture, implementation notes and experiments).

> Educational runtime, **not a hardened sandbox**. Run it in a disposable Ubuntu VM.

## Quick start (Ubuntu 22.04/24.04 VM, cgroups v2)

```bash
scripts/setup_environment.sh          # packages + kernel feature check
scripts/setup_rootfs.sh               # ./rootfs = static BusyBox + test workloads
cmake -B build && cmake --build build -j
sudo ./build/minicontainer run --name demo --rootfs ./rootfs \
     --memory 100M --cpu 50 --hostname isolated -- /bin/sh
```

Inside the container try `hostname`, `ps`, `ls /`, `cat /proc/self/cgroup`. Exit the shell to see the resource summary.

## Commands

| Command | Purpose |
|---|---|
| `run [options] -- cmd...` | start a container (foreground, or `-d` background); returns the command's exit status |
| `ps` | list running containers |
| `inspect NAME` | PID, namespaces (isolated vs host), limits, live memory/CPU/PIDs usage |
| `stop NAME` | SIGTERM, then SIGKILL after `--grace` seconds; cleans up |
| `logs NAME` | output of a detached container |
| `clean` | remove stale records/cgroups/logs (e.g. after a runner was `kill -9`'d) |

Run options: `--rootfs DIR` (required) `--name` `--hostname` `--memory 100M` `--cpu 50` `--pids N` `--isolate-net`
`--caps default|strict|all` `--grace N` `--no-init` `-d` `-q` `--stats-file F`. See `minicontainer --help`.

## Test it

```bash
ctest --test-dir build                 # unit tests (+ end-to-end ones when run as root with a rootfs)
sudo scripts/run_tests.sh              # full acceptance suite AC-01..AC-10, signals, stress, cleanup
sudo scripts/run_experiments.sh        # PRD experiments 1-5 -> results/*.csv
python3 scripts/plot_results.py        # -> results/*.png for the report
```

## Layout
`include/` + `src/` per module (cli, container, process, namespace, resource, filesystem, common), `tests/`,
`scripts/`, `tools/` (static test workloads: memhog, cpuburn, pidtest), `docs/`.
