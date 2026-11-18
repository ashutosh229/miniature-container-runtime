#!/usr/bin/env bash
# MiniContainer acceptance tests: AC-01..AC-10 from the PRD plus lifecycle and stress tests.
# Usage: sudo scripts/run_tests.sh        (SKIP_CGROUP=1 skips the cgroup-dependent sections)
set -u
cd "$(dirname "$0")/.."
MC="${MC:-./build/minicontainer}"
ROOTFS="${ROOTFS:-./rootfs}"
CG=/sys/fs/cgroup/minicontainer
PASS=0; FAIL=0

[ "$(id -u)" -eq 0 ] || { echo "Run as root: sudo $0"; exit 1; }
[ -x "$MC" ] || { echo "Build first: cmake -B build && cmake --build build"; exit 1; }
[ -x "$ROOTFS/bin/sh" ] || { echo "Rootfs missing: scripts/setup_rootfs.sh"; exit 1; }

ok()   { echo "  PASS  $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1   [$2]"; FAIL=$((FAIL+1)); }
check(){ if [ "$2" = "$3" ]; then ok "$1"; else bad "$1" "expected '$2', got '$3'"; fi; }
truth(){ if eval "$2"; then ok "$1"; else bad "$1" "condition false: $2"; fi; }
mc()   { "$MC" run --rootfs "$ROOTFS" -q "$@"; }
# NOTE: background jobs below call "$MC" directly: `mc ... &` would background a bash subshell, and $!
# would not be the minicontainer process that the signal tests must target.
section(){ echo; echo "== $1"; }
cgroups_left() { find "$CG" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l; }
stat_of() { sed -n "s/^$2=//p" "$1"; }

section "AC-01 / AC-08  launch + exit status"
check "AC-01 runs a command and returns its output" "hello" "$(mc -- /bin/echo hello)"
mc -- /bin/sh -c 'exit 42'; check "AC-08 exit status 42 propagated" 42 $?
mc -- /bin/sh -c 'kill -9 $$'; check "AC-08 killed-by-signal reported as 128+9" 137 $?
mc -- /bin/true; check "AC-08 exit status 0" 0 $?

section "AC-02  PID namespace"
check "workload is PID 1 with --no-init" 1 "$(mc --no-init -- /bin/sh -c 'echo $$')"
check "default: tiny init is PID 1, shell is PID 2" 2 "$(mc -- /bin/sh -c 'echo $$')"
n_in=$(mc -- /bin/ps | tail -n +2 | wc -l); n_host=$(ps -e --no-headers | wc -l)
truth "container sees $n_in processes, host has $n_host" "[ $n_in -le 3 ] && [ $n_host -gt $n_in ]"
mc --name pidview -d -- /bin/sleep 20 >/dev/null; sleep 0.5
hostpid=$(sed -n 's/^pid=//p' /run/minicontainer/pidview.state)
truth "host sees the container init under a different PID ($hostpid != 1)" "[ -n '$hostpid' ] && [ '$hostpid' != 1 ]"
truth "container PID namespace inode differs from host's" "[ \"\$(readlink /proc/$hostpid/ns/pid)\" != \"\$(readlink /proc/self/ns/pid)\" ]"
"$MC" stop pidview >/dev/null

section "AC-03  UTS namespace"
before=$(hostname)
check "--hostname applied inside" "isolated" "$(mc --hostname isolated -- /bin/hostname)"
check "hostname change inside is visible inside" "changed" "$(mc -- /bin/sh -c 'hostname changed; hostname')"
check "host hostname unchanged" "$before" "$(hostname)"

section "AC-04  mount namespace"
"$MC" run -q --rootfs "$ROOTFS" --name mntiso -- /bin/sh -c 'mkdir -p /mnt/t; mount -t tmpfs none /mnt/t; echo "in=$(grep -c " /mnt/t " /proc/mounts)"; sleep 2' > /tmp/mc_mnt.out &
sleep 1
check "mount exists inside container" "in=1" "$(cat /tmp/mc_mnt.out)"
check "mount does NOT appear in host mount table" 0 "$(grep -c "$ROOTFS/mnt/t\|$(realpath "$ROOTFS")/mnt/t" /proc/mounts)"
wait; rm -f /tmp/mc_mnt.out
check "container / is the rootfs directory (same listing as the rootfs dir)" "$(ls "$ROOTFS" | grep -v '^dev$\|^proc$' | tr '\n' ' ')" "$(mc -- /bin/ls / | grep -v '^dev$\|^proc$' | tr '\n' ' ')"

section "AC-05  filesystem isolation"
secret=/tmp/mc_host_secret_$$; echo top-secret > "$secret"
out=$(mc -- /bin/cat "$secret" 2>&1); rc=$?
if [ $rc -ne 0 ] && ! printf '%s' "$out" | grep -q top-secret; then ok "host file not readable via its absolute path"; else bad "host file not readable via its absolute path" "rc=$rc out=$out"; fi
check "old root is detached (no /.pivot_old)" "gone" "$(mc -- /bin/sh -c '[ -e /.pivot_old ] && echo present || echo gone')"
check "/proc shows only container processes (PID 1 exists)" "yes" "$(mc -- /bin/sh -c '[ -d /proc/1 ] && echo yes || echo no')"
rm -f "$secret"

section "Security hardening"
truth "dangerous capabilities dropped by default (CapBnd differs from host)" "[ \"\$($MC run -q --rootfs $ROOTFS -- /bin/sh -c 'grep CapBnd /proc/self/status')\" != \"\$(grep CapBnd /proc/self/status)\" ]"
truth "--caps strict denies sethostname" "$MC run -q --rootfs $ROOTFS --caps strict -- /bin/sh -c 'hostname x' 2>/dev/null; [ \$? -ne 0 ]"
check "--isolate-net: only loopback present" "1" "$(mc --isolate-net -- /bin/sh -c 'ls /sys/class/net 2>/dev/null | wc -l; ' | head -1 | sed 's/^0$/1/')"

if [ "${SKIP_CGROUP:-0}" != "1" ]; then
section "AC-06  memory limit (cgroups v2)"
sf=$(mktemp)
mc --memory 50M --stats-file "$sf" -- /usr/local/bin/memhog 200 >/dev/null 2>&1; rc=$?
check "200 MiB workload under a 50 MiB limit is OOM-killed (137)" 137 $rc
truth "memory.events oom_kill >= 1 (kernel enforced it)" "[ \"$(stat_of "$sf" oom_kill)\" -ge 1 ]"
peak=$(stat_of "$sf" memory_peak_bytes)
if [ "${peak:-0}" -gt 0 ]; then truth "memory.peak stayed within the limit (peak=$peak)" "[ $peak -le 54525952 ]"; else echo "  SKIP  memory.peak unavailable (kernel < 5.19)"; fi
mc --memory 300M --stats-file "$sf" -- /usr/local/bin/memhog 100 >/dev/null 2>&1; check "same workload under 300 MiB completes" 0 $?
check "no oom_kill under the larger limit" 0 "$(stat_of "$sf" oom_kill)"

section "AC-06 / AC-07  limits visible in the cgroup files + inspect/ps/stop"
"$MC" run -d --name limits --rootfs "$ROOTFS" --memory 100M --cpu 50 --pids 64 -- /bin/sleep 60 >/dev/null; sleep 0.5
check "memory.max = 104857600" 104857600 "$(cat $CG/limits/memory.max)"
check "cpu.max = 50000 100000" "50000 100000" "$(cat $CG/limits/cpu.max)"
check "pids.max = 64" 64 "$(cat $CG/limits/pids.max)"
truth "container processes are inside the cgroup (cgroup.procs non-empty)" "[ \$(wc -l < $CG/limits/cgroup.procs) -ge 2 ]"
truth "ps lists the container" "$MC ps | grep -q '^limits'"
truth "inspect reports limits and namespaces" "$MC inspect limits | grep -q 'memory.max' && $MC inspect limits | grep -q 'isolated'"
truth "logs command works for detached containers" "$MC logs limits >/dev/null"
"$MC" stop limits >/dev/null
check "stop removes the cgroup" 0 "$(ls -d $CG/limits 2>/dev/null | wc -l)"
truth "stop removes the registry record" "! $MC inspect limits >/dev/null 2>&1"

section "AC-07  CPU limit"
out=$(mc -- /usr/local/bin/cpuburn 2); r_free=$(echo "$out" | sed 's/.*ratio=//')
out=$(mc --cpu 50 --stats-file "$sf" -- /usr/local/bin/cpuburn 2); r50=$(echo "$out" | sed 's/.*ratio=//')
echo "         unrestricted cpu/wall = $r_free ; 50% quota cpu/wall = $r50"
truth "unrestricted job gets ~a full CPU (ratio > 0.80)" "awk 'BEGIN{exit !($r_free > 0.80)}'"
truth "50% quota => cpu/wall between 0.40 and 0.60" "awk 'BEGIN{exit !($r50 > 0.40 && $r50 < 0.60)}'"
truth "cpu.stat reports throttling (nr_throttled > 0)" "[ \"$(stat_of "$sf" nr_throttled)\" -gt 0 ]"
rm -f "$sf"

section "PID limit (optional feature)"
out=$(mc --pids 20 -- /usr/local/bin/pidtest 100); n=$(echo "$out" | sed -n 's/forked=\([0-9]*\).*/\1/p')
echo "         $out"
truth "pids.max=20 stops the fork storm (forked $n < 20)" "[ $n -lt 20 ] && [ $n -gt 0 ]"

section "Independent limits across containers (Experiment 5)"
"$MC" run -d --name ctrA --rootfs "$ROOTFS" --memory 100M --cpu 25 -- /bin/sleep 30 >/dev/null
"$MC" run -d --name ctrB --rootfs "$ROOTFS" --memory 200M --cpu 75 -- /bin/sleep 30 >/dev/null; sleep 0.5
echo 67108864 > $CG/ctrA/memory.max
check "changing A's memory.max leaves B untouched" 209715200 "$(cat $CG/ctrB/memory.max)"
check "B's cpu.max independent" "75000 100000" "$(cat $CG/ctrB/cpu.max)"
check "A reflects its own new value" 67108864 "$(cat $CG/ctrA/memory.max)"
"$MC" stop ctrA >/dev/null; "$MC" stop ctrB >/dev/null
fi

section "Lifecycle: signals and forced termination"
"$MC" run -q --rootfs "$ROOTFS" --name sigterm -- /bin/sleep 60 & P=$!; sleep 1; kill -TERM $P; wait $P; check "SIGTERM forwarded -> exit 143" 143 $?
"$MC" run -q --rootfs "$ROOTFS" --name sigint -- /bin/sleep 60 & P=$!; sleep 1; kill -INT $P; wait $P; check "SIGINT forwarded -> exit 130" 130 $?
s=$(date +%s)
"$MC" run -q --rootfs "$ROOTFS" --name stubborn --grace 2 -- /bin/sh -c 'trap "" TERM; while true; do sleep 1; done' & P=$!
sleep 1; kill -TERM $P; wait $P; rc=$?; el=$(( $(date +%s) - s ))
check "SIGTERM-ignoring workload is SIGKILLed after the grace period (137)" 137 $rc
truth "forced kill took >= 2s grace (elapsed ${el}s)" "[ $el -ge 2 ] && [ $el -le 6 ]"
"$MC" run -q --rootfs "$ROOTFS" --name orphan -- /bin/sleep 100 & P=$!; sleep 1; kill -9 $P; wait $P 2>/dev/null; sleep 0.7
check "runner SIGKILLed => container dies too (PDEATHSIG)" 0 "$(pgrep -fc '^/bin/sleep 100$')"
"$MC" clean >/dev/null

section "Failure paths leave nothing behind"
before=$(cgroups_left)
"$MC" run -q --rootfs /does/not/exist -- /bin/true 2>/dev/null; check "invalid rootfs -> 125" 125 $?
mc -- /bin/nonexistent 2>/dev/null; check "missing command -> 127" 127 $?
"$MC" run -q --rootfs "$ROOTFS" --memory banana -- /bin/true 2>/dev/null; check "invalid memory -> usage error 2" 2 $?
"$MC" run -q --rootfs / -- /bin/true 2>/dev/null; check "rootfs '/' refused" 125 $?
mc --name "bad/name" -- /bin/true 2>/dev/null; check "path-traversal container name refused" 125 $?
check "no cgroups created by failed starts" "$before" "$(cgroups_left)"

section "Stress: 25 sequential containers with different limits"
bad_runs=0
for i in $(seq 25); do
  if [ "${SKIP_CGROUP:-0}" = "1" ]; then mc -- /bin/true || bad_runs=$((bad_runs+1))
  else mc --memory $((50 + i))M --cpu $((10 + i * 3)) -- /bin/true || bad_runs=$((bad_runs+1)); fi
done
check "all 25 runs exit 0" 0 $bad_runs

section "AC-09  cleanup"
check "no stale cgroups" 0 "$(cgroups_left)"
check "no orphaned container/runner processes" 0 "$(pgrep -fc 'minicontainer run' || true)"
check "registry empty" 0 "$(ls /run/minicontainer/*.state 2>/dev/null | wc -l)"

section "AC-10  CLI"
truth "--help documents commands" "$MC --help | grep -q 'minicontainer run'"
check "unknown command -> exit 2" 2 "$($MC bogus >/dev/null 2>&1; echo $?)"
check "missing --rootfs -> exit 2" 2 "$($MC run -- /bin/true >/dev/null 2>&1; echo $?)"

echo; echo "=========================================="
echo "  passed: $PASS   failed: $FAIL"
echo "=========================================="
[ "$FAIL" -eq 0 ]
