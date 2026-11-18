#!/usr/bin/env python3
"""Turn results/*.csv into PNG graphs for the report (needs matplotlib: sudo apt install python3-matplotlib)."""
import csv, os, statistics, sys
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    sys.exit("matplotlib missing: sudo apt install python3-matplotlib")

R = "results"
def rows(name):
    with open(os.path.join(R, name)) as f:
        return list(csv.DictReader(f))

def save(fig, name):
    fig.tight_layout(); fig.savefig(os.path.join(R, name), dpi=150); plt.close(fig); print("wrote", name)

# Exp 1a: native vs container CPU job
d = rows("exp1_cpu.csv")
modes = ["native", "container"]
wall = [statistics.mean(float(r["wall_s"]) for r in d if r["mode"] == m) for m in modes]
fig, ax = plt.subplots(figsize=(4, 3.2)); ax.bar(modes, wall, color=["#888", "#3b7"])
ax.set_ylabel("elapsed seconds (2 CPU-s job)"); ax.set_title("Exp 1: native vs container"); save(fig, "exp1_cpu.png")

# Exp 1b: startup latency
d = rows("exp1_startup.csv")
vals = [[float(r["startup_ms"]) for r in d if r["mode"] == m] for m in modes]
fig, ax = plt.subplots(figsize=(4, 3.2)); ax.boxplot(vals, labels=modes)
ax.set_ylabel("ms to run /bin/true"); ax.set_title("Exp 1: startup latency"); save(fig, "exp1_startup.png")

# Exp 2: CPU quota
d = rows("exp2_cpu_quota.csv")
q = [int(r["quota_percent"]) for r in d]
fig, (a1, a2) = plt.subplots(1, 2, figsize=(8, 3.2))
a1.plot(q, [float(r["cpu_share"]) * 100 for r in d], "o-", label="measured"); a1.plot(q, q, "k--", label="ideal")
a1.set_xlabel("cpu quota (%)"); a1.set_ylabel("CPU consumption (% of one CPU)"); a1.legend()
a2.plot(q, [float(r["wall_s"]) for r in d], "o-"); a2.set_xlabel("cpu quota (%)"); a2.set_ylabel("elapsed seconds")
fig.suptitle("Exp 2: CPU quota"); save(fig, "exp2_cpu_quota.png")

# Exp 3: memory
d = rows("exp3_memory.csv")
fig, ax = plt.subplots(figsize=(5, 3.2))
cols = ["#3b7" if r["completed"] == "yes" else "#c44" for r in d]
ax.bar([r["limit"] for r in d], [float(r["peak_mib"]) for r in d], color=cols)
ax.set_ylabel("peak memory (MiB)"); ax.set_xlabel("memory limit (green = completed, red = OOM-killed)")
ax.set_title("Exp 3: memory limit (150 MiB workload)"); save(fig, "exp3_memory.png")
