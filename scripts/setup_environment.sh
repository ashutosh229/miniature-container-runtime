#!/usr/bin/env bash
# Installs build dependencies on Ubuntu and checks the kernel features MiniContainer needs.
set -euo pipefail

echo "== Installing packages =="
sudo apt-get update -y
sudo apt-get install -y build-essential cmake gdb strace git busybox-static procps stress-ng python3-matplotlib

echo "== Checking kernel features =="
fail=0
if [ "$(stat -fc %T /sys/fs/cgroup)" = "cgroup2fs" ]; then
  echo "  OK    cgroups v2 mounted at /sys/fs/cgroup"
else
  echo "  FAIL  /sys/fs/cgroup is not cgroup2. On Ubuntu 22.04+/24.04 it is the default."
  echo "        (If you are on an older release add 'systemd.unified_cgroup_hierarchy=1' to the kernel command line.)"
  fail=1
fi
for c in memory cpu pids; do
  if grep -qw "$c" /sys/fs/cgroup/cgroup.controllers 2>/dev/null; then echo "  OK    controller: $c"; else echo "  WARN  controller missing: $c"; fi
done
for ns in pid uts mnt ipc net; do
  [ -e /proc/self/ns/$ns ] && echo "  OK    namespace: $ns" || { echo "  FAIL  namespace: $ns"; fail=1; }
done
if grep -qi microsoft /proc/version; then
  echo "  WARN  WSL detected. cgroup/namespace behaviour differs; the PRD recommends a dedicated Ubuntu VM."
fi
echo "  kernel: $(uname -r)  (memory.peak needs >= 5.19, cgroup.kill needs >= 5.14; both are optional)"
[ $fail -eq 0 ] && echo "Environment looks good." || { echo "Environment has problems (see above)."; exit 1; }
