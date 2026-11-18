#!/usr/bin/env bash
# Builds ./rootfs: a minimal static-BusyBox root filesystem plus the test workloads from tools/.
# Usage: scripts/setup_rootfs.sh [rootfs_dir]
set -euo pipefail
cd "$(dirname "$0")/.."
ROOTFS="${1:-./rootfs}"

BB="$(command -v busybox || true)"
if [ -z "$BB" ]; then echo "busybox not found: sudo apt install busybox-static"; exit 1; fi
if ldd "$BB" 2>&1 | grep -q "=>"; then
  echo "$BB is dynamically linked. Install a static one: sudo apt install busybox-static"; exit 1
fi

echo "Creating rootfs in $ROOTFS"
mkdir -p "$ROOTFS"/{bin,sbin,usr/bin,usr/sbin,usr/local/bin,etc,proc,sys,dev,tmp,root,mnt,var/tmp,home}
chmod 1777 "$ROOTFS/tmp" "$ROOTFS/var/tmp"
cp "$BB" "$ROOTFS/bin/busybox"
# Create an applet symlink for every BusyBox command (/bin/sh -> busybox, /bin/ps -> busybox, ...).
for applet in $("$ROOTFS/bin/busybox" --list); do
  [ -e "$ROOTFS/bin/$applet" ] || ln -s busybox "$ROOTFS/bin/$applet"
done
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$ROOTFS/etc/passwd"
printf 'root:x:0:\n' > "$ROOTFS/etc/group"
printf 'container\n' > "$ROOTFS/etc/hostname"
printf '127.0.0.1 localhost\n' > "$ROOTFS/etc/hosts"
printf 'export PS1="\\u@\\h:\\w# "\n' > "$ROOTFS/etc/profile"

# Static test workloads (memory hog, CPU burner, fork tester) for the experiments.
if command -v gcc >/dev/null; then
  for t in memhog cpuburn pidtest; do
    gcc -O2 -static -o "$ROOTFS/usr/local/bin/$t" "tools/$t.c" && echo "  built $t"
  done
else
  echo "gcc not found: skipping test workloads (memhog/cpuburn/pidtest)"
fi
echo "rootfs ready: $(du -sh "$ROOTFS" | cut -f1)"
