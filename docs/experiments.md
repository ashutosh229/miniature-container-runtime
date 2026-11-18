# Experiments (PRD section 12)

Run in a disposable Ubuntu VM (2-4 CPUs, idle otherwise):

```bash
sudo REPS=5 scripts/run_experiments.sh
python3 scripts/plot_results.py
```

| # | Question | Output | Expected |
|---|---|---|---|
| 1 | Native vs. containerized cost | `exp1_cpu.csv`, `exp1_startup.csv`, `exp1_memory_overhead.csv` | same elapsed/CPU time for a CPU job; startup = a few ms; memory overhead = init + page tables, ~1 MiB |
| 2 | CPU quota 100/75/50/25 % | `exp2_cpu_quota.csv` | CPU share tracks the quota; elapsed time ~ 1/quota; `nr_throttled` grows as quota shrinks |
| 3 | Memory limit none/200M/100M/50M with a 150 MiB job | `exp3_memory.csv` | completes under none/200M; OOM-killed (137, `oom_kill>=1`) under 100M/50M; peak <= limit |
| 4 | Namespace views | `exp4_namespaces.txt` | different ns inode numbers, hostname, process count, mount table |
| 5 | Independent containers | `exp5_multi.txt` | editing A's limits leaves B's `memory.max`/`cpu.max` unchanged |

## Results (fill in after running on your VM)
Paste the graphs (`results/*.png`) and a short analysis per experiment: what you measured, whether it matches the table above, and why
(e.g. CFS bandwidth control: quota/period = 50000/100000 lets the group run 50 ms per 100 ms, so a 2 CPU-second job needs ~4 s wall time).

## Manual demo script for the viva
```bash
sudo ./build/minicontainer run --name demo --rootfs ./rootfs --memory 100M --cpu 50 --hostname isolated -- /bin/sh
#   in the container:  hostname ; ps ; hostname changed ; ls / ; cat /tmp/anything-from-host (fails) ; mount | head
# second terminal:
sudo ./build/minicontainer ps ; sudo ./build/minicontainer inspect demo
cat /sys/fs/cgroup/minicontainer/demo/{memory.max,cpu.max}
sudo ./build/minicontainer stop demo
# resource enforcement:
sudo ./build/minicontainer run --rootfs ./rootfs --memory 50M -- /usr/local/bin/memhog 200       # OOM-killed, exit 137
sudo ./build/minicontainer run --rootfs ./rootfs --cpu 50 -- /usr/local/bin/cpuburn 2            # wall ~4s for 2s CPU
```
