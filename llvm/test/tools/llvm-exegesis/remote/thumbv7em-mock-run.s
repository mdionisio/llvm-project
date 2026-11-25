# REQUIRES: arm-registered-target

# Check the remote execution mode on M-profile against the mock helper. The
# return address in LR has the Thumb bit set and xPSR.T is set before each
# run; the breakpoint stays on the even return pad address.

# RUN: rm -f %t.log
# RUN: llvm-exegesis --mtriple=thumbv7em-none-eabi --mcpu=cortex-m7 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/thumbv7em.yaml \
# RUN:   --remote-helper='%exegesis_mock_helper --function 0x101000 --results 0x20010000 --width 32 --log %t.log' \
# RUN:   --repetition-mode=middle-half-duplicate --min-instructions=1000 \
# RUN:   --snippets-file=%s | FileCheck %s
# RUN: FileCheck %s --check-prefix=LOG < %t.log

# A 24-bit down counter with a scale (SysTick on QEMU): the mock starts it
# close to zero, so it wraps, and each count is worth 40 cycles.
# RUN: llvm-exegesis --mtriple=thumbv7em-none-eabi --mcpu=cortex-m7 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/thumbv7em-systick.yaml \
# RUN:   --remote-helper='%exegesis_mock_helper --function 0x101000 --results 0x20010000 --down --start 5 --width 24' \
# RUN:   --repetition-mode=middle-half-duplicate --min-instructions=1000 \
# RUN:   --snippets-file=%s | FileCheck %s --check-prefix=SYSTICK

# LLVM-EXEGESIS-DEFREG R1 0
# LLVM-EXEGESIS-DEFREG R2 0
add.w r1, r1, r2

# CHECK:      mode: latency
# CHECK:      llvm_triple: thumbv7em-unknown-none-eabi
# CHECK:      measurements:
# CHECK-NEXT:   - { key: latency, value: 1, per_snippet_value: 1
# CHECK:      error: ''

# LOG:      hello 1
# LOG-NEXT: write 0x20000000 <65536 bytes>
# LOG-NEXT: write 0x100000 <2 bytes>
# LOG-NEXT: write 0x101000 <{{[0-9]+}} bytes>
# LOG-NEXT: write 0x20010000 <16 bytes>
# LOG-NEXT: run 0x101000 0x100000 2000 r0=0x20000000 sp=0x20100000 lr=0x100001 xpsr=0x1000000
# LOG-NEXT: read 0x20010000 0x10
# LOG:      quit

# SYSTICK:      measurements:
# SYSTICK-NEXT:   - { key: latency, value: 40, per_snippet_value: 40
# SYSTICK:      error: ''
