# REQUIRES: arm-registered-target

# Check the counter reads of the remote execution mode on M-profile: DWT
# CYCCNT, enabled by the first read (which also enables the FPU), behind
# dsb/isb. The 32-bit value goes in
# the low word of each 8-byte slot of the results area, the high word is
# zeroed.

# RUN: llvm-exegesis --mtriple=thumbv7em-none-eabi --mcpu=cortex-m7 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/thumbv7em.yaml \
# RUN:   --remote-helper=unused --benchmark-phase=assemble-measured-code \
# RUN:   --repetition-mode=duplicate --min-instructions=2 \
# RUN:   --dump-object-to-disk=%t.o --snippets-file=%s > /dev/null
# RUN: llvm-objdump -d --triple=thumbv7em --mcpu=cortex-m7 %t.o | FileCheck %s

# The same reads with a memory-mapped counter from the description (SysTick):
# its setup stores come first.
# RUN: llvm-exegesis --mtriple=thumbv7em-none-eabi --mcpu=cortex-m7 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/thumbv7em-systick.yaml \
# RUN:   --remote-helper=unused --benchmark-phase=assemble-measured-code \
# RUN:   --repetition-mode=duplicate --min-instructions=2 \
# RUN:   --dump-object-to-disk=%t.systick.o --snippets-file=%s > /dev/null
# RUN: llvm-objdump -d --triple=thumbv7em --mcpu=cortex-m7 %t.systick.o \
# RUN:   | FileCheck %s --check-prefix=SYSTICK

# LLVM-EXEGESIS-DEFREG R1 0
# LLVM-EXEGESIS-DEFREG R2 0
add.w r1, r1, r2

# CPACR: FPU access, then DEMCR.TRCENA, DWT_CTRL.CYCCNTENA
# CHECK:      movw r3, #0xed88
# CHECK-NEXT: movt r3, #0xe000
# CHECK-NEXT: ldr.w r12, [r3]
# CHECK-NEXT: orr r12, r12, #0xf00000
# CHECK-NEXT: str.w r12, [r3]
# CHECK-NEXT: movw r3, #0xedfc
# CHECK-NEXT: movt r3, #0xe000
# CHECK-NEXT: ldr.w r12, [r3]
# CHECK-NEXT: orr r12, r12, #0x1000000
# CHECK-NEXT: str.w r12, [r3]
# CHECK-NEXT: movw r3, #0x1000
# CHECK-NEXT: movt r3, #0xe000
# CHECK-NEXT: ldr.w r12, [r3]
# CHECK-NEXT: orr r12, r12, #0x1
# CHECK-NEXT: str.w r12, [r3]
# Start read: CYCCNT -> results[0]
# CHECK-NEXT: movw r3, #0x1004
# CHECK-NEXT: movt r3, #0xe000
# CHECK-NEXT: dsb sy
# CHECK-NEXT: isb sy
# CHECK-NEXT: ldr.w r12, [r3]
# CHECK-NEXT: movw r3, #0x0
# CHECK-NEXT: movt r3, #0x2001
# CHECK-NEXT: str.w r12, [r3]
# CHECK-NEXT: movw r12, #0x0
# CHECK-NEXT: str.w r12, [r3, #0x4]
# Register setup, snippet.
# CHECK-NEXT: movw r1, #0x0
# CHECK-NEXT: movw r2, #0x0
# CHECK-NEXT: add.w r1, r1, r2
# CHECK-NEXT: add.w r1, r1, r2
# End read: CYCCNT -> results[1]
# CHECK-NEXT: movw r3, #0x1004
# CHECK-NEXT: movt r3, #0xe000
# CHECK-NEXT: dsb sy
# CHECK-NEXT: isb sy
# CHECK-NEXT: ldr.w r12, [r3]
# CHECK-NEXT: movw r3, #0x8
# CHECK-NEXT: movt r3, #0x2001
# CHECK-NEXT: str.w r12, [r3]
# CHECK-NEXT: movw r12, #0x0
# CHECK-NEXT: str.w r12, [r3, #0x4]
# CHECK-NEXT: bx lr

# CPACR, then SYST_RVR = 0xffffff, SYST_CSR = 5
# SYSTICK:      movw r3, #0xed88
# SYSTICK:      str.w r12, [r3]
# SYSTICK-NEXT: movw r3, #0xe014
# SYSTICK-NEXT: movt r3, #0xe000
# SYSTICK-NEXT: movw r12, #0xffff
# SYSTICK-NEXT: movt r12, #0xff
# SYSTICK-NEXT: str.w r12, [r3]
# SYSTICK-NEXT: movw r3, #0xe010
# SYSTICK-NEXT: movt r3, #0xe000
# SYSTICK-NEXT: movw r12, #0x5
# SYSTICK-NEXT: str.w r12, [r3]
# SYSTICK-NEXT: movw r3, #0xe018
# SYSTICK-NEXT: movt r3, #0xe000
# SYSTICK-NEXT: dsb sy
# SYSTICK-NEXT: isb sy
# SYSTICK-NEXT: ldr.w r12, [r3]
# SYSTICK:      add.w r1, r1, r2
# SYSTICK-NEXT: add.w r1, r1, r2
# SYSTICK-NEXT: movw r3, #0xe018
# SYSTICK-NEXT: movt r3, #0xe000
# SYSTICK-NEXT: dsb sy
# SYSTICK-NEXT: isb sy
# SYSTICK-NEXT: ldr.w r12, [r3]
# SYSTICK:      bx lr
