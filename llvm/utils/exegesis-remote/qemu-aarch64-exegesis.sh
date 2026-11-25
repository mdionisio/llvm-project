#!/bin/sh
# Runs llvm-exegesis in the remote execution mode against QEMU aarch64 virt.
# Usage: qemu-aarch64-exegesis.sh <path/to/llvm-exegesis> [llvm-exegesis args]
# With -icount shift=0 the "cycles" are instructions: this checks the
# plumbing, not the timing.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
EXEGESIS=$1; shift
PORT=${QEMU_GDB_PORT:-1234}
qemu-system-aarch64 -M virt -cpu cortex-a53 -m 512M -nographic \
  -monitor none -serial none -icount shift=0 -S -gdb tcp::"$PORT" &
QEMU=$!
trap 'kill $QEMU 2>/dev/null' EXIT
sleep 0.5
"$EXEGESIS" --mtriple=aarch64 --mcpu=cortex-a53 \
  --execution-mode=remote \
  --remote-target="$HERE/qemu-aarch64-virt.yaml" \
  --remote-helper="python3 $HERE/exegesis-gdb-remote-helper.py --port $PORT" \
  "$@"
