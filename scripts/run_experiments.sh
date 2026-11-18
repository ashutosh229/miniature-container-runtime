#!/usr/bin/env bash
# Runs PRD Experiments 1-5 and writes CSV/text results to ./results (plot with scripts/plot_results.py).
# Usage: sudo REPS=5 scripts/run_experiments.sh
set -eu
cd "$(dirname "$0")/.."
MC="${MC:-./build/minicontainer}"; ROOTFS="${ROOTFS:-./rootfs}"; REPS="${REPS:-5}"
OUT=results; CG=/sys/fs/cgroup/minicontainer
[ "$(id -u)" -eq 0 ] || { echo "Run as root"; exit 1; }
mkdir -p "$OUT"
BURN="$ROOTFS/usr/local/bin/cpuburn"; HOG=/usr/local/bin/memhog
field() { sed -n "s/.*$2=\([0-9.]*\).*/\1/p" <<<"$1"; }
sf=$(mktemp); trap 'rm -f "$sf"' EXIT
mc() { "$MC" run -q --rootfs "$ROOTFS" "$@"; }
sv() { sed -n "s/^$1=//p" "$sf"; }

echo "== Experiment 1: native vs containerized (CPU job of 2 CPU-seconds, x$REPS)"
echo "mode,run,wall_s,cpu_s" > $OUT/exp1_cpu.csv
for i in $(seq "$REPS"); do
  o=$("$BURN" 2);                         echo "native,$i,$(field "$o" wall),$(field "$o" cpu)" >> $OUT/exp1_cpu.csv
  o=$(mc -- /usr/local/bin/cpuburn 2);    echo "container,$i,$(field "$o" wall),$(field "$o" cpu)" >> $OUT/exp1_cpu.csv
done
echo "mode,run,startup_ms" > $OUT/exp1_startup.csv
for i in $(seq 20); do
  s=$(date +%s%N); /bin/true; e=$(date +%s%N); echo "native,$i,$(awk "BEGIN{print ($e-$s)/1e6}")" >> $OUT/exp1_startup.csv
  s=$(date +%s%N); mc -- /bin/true; e=$(date +%s%N); echo "container,$i,$(awk "BEGIN{print ($e-$s)/1e6}")" >> $OUT/exp1_startup.csv
done
mc --stats-file "$sf" -- /bin/true
echo "container_memory_peak_bytes,$(sv memory_peak_bytes)" > $OUT/exp1_memory_overhead.csv
echo "container_startup_ms_internal,$(awk "BEGIN{print $(sv startup_usec)/1000}")" >> $OUT/exp1_memory_overhead.csv

echo "== Experiment 2: CPU quota sweep"
echo "quota_percent,wall_s,cpu_s,cpu_share,nr_throttled" > $OUT/exp2_cpu_quota.csv
for q in 100 75 50 25; do
  o=$(mc --cpu $q --stats-file "$sf" -- /usr/local/bin/cpuburn 2)
  echo "$q,$(field "$o" wall),$(field "$o" cpu),$(field "$o" ratio),$(sv nr_throttled)" >> $OUT/exp2_cpu_quota.csv
done

echo "== Experiment 3: memory limit sweep (memhog 150 MiB)"
echo "limit,exit_code,completed,peak_mib,oom_kill,limit_hits" > $OUT/exp3_memory.csv
for lim in none 200M 100M 50M; do
  args=(); [ "$lim" != none ] && args=(--memory "$lim")
  set +e; mc "${args[@]}" --stats-file "$sf" -- $HOG 150 >/dev/null 2>&1; rc=$?; set -e
  done_=no; [ $rc -eq 0 ] && done_=yes
  echo "$lim,$rc,$done_,$(awk "BEGIN{print $(sv memory_peak_bytes)/1048576}"),$(sv oom_kill),$(sv memory_limit_hits)" >> $OUT/exp3_memory.csv
done

echo "== Experiment 4: namespace comparison"
{
  echo "--- host ---";      echo "hostname: $(hostname)"; echo "processes: $(ps -e --no-headers | wc -l)"
  echo "mounts: $(wc -l < /proc/mounts)"; for n in pid uts mnt ipc net; do echo "ns/$n: $(readlink /proc/self/ns/$n)"; done
  echo "--- container ---"
  mc --hostname isolated -- /bin/sh -c 'echo "hostname: $(hostname)"; echo "processes: $(ps | tail -n +2 | wc -l)"; echo "mounts: $(wc -l < /proc/mounts)"; for n in pid uts mnt ipc net; do echo "ns/$n: $(readlink /proc/self/ns/$n)"; done'
} | tee $OUT/exp4_namespaces.txt

echo "== Experiment 5: multiple containers with independent limits"
"$MC" run -d --name expA --rootfs "$ROOTFS" --memory 100M --cpu 25 -- /bin/sleep 30 >/dev/null
"$MC" run -d --name expB --rootfs "$ROOTFS" --memory 200M --cpu 75 -- /bin/sleep 30 >/dev/null; sleep 0.5
{
  echo "initial:  A memory.max=$(cat $CG/expA/memory.max) cpu.max=$(cat $CG/expA/cpu.max) | B memory.max=$(cat $CG/expB/memory.max) cpu.max=$(cat $CG/expB/cpu.max)"
  echo 67108864 > $CG/expA/memory.max; echo "20000 100000" > $CG/expA/cpu.max
  echo "after A changed: A memory.max=$(cat $CG/expA/memory.max) cpu.max=$(cat $CG/expA/cpu.max) | B memory.max=$(cat $CG/expB/memory.max) cpu.max=$(cat $CG/expB/cpu.max)"
} | tee $OUT/exp5_multi.txt
"$MC" stop expA >/dev/null; "$MC" stop expB >/dev/null
echo "Done. Results in $OUT/. Next: python3 scripts/plot_results.py"
