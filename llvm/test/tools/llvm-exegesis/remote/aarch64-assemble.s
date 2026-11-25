# Check the counter reads that the remote execution mode adds around the
# snippet: setup and first read before the register setup, second read before
# the return. No helper is needed to assemble.

# RUN: llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/aarch64.yaml \
# RUN:   --remote-helper=unused --benchmark-phase=assemble-measured-code \
# RUN:   --repetition-mode=duplicate --min-instructions=2 \
# RUN:   --dump-object-to-disk=%t.o --snippets-file=%s > /dev/null
# RUN: llvm-objdump -d %t.o | FileCheck %s

# LLVM-EXEGESIS-DEFREG X1 0
# LLVM-EXEGESIS-DEFREG X2 0
add x1, x1, x2

# CHECK:      msr CPACR_EL1, x16
# CHECK:      msr PMCR_EL0, x16
# CHECK:      msr PMCNTENSET_EL0, x16
# CHECK:      msr PMCCFILTR_EL0, xzr
# CHECK-NEXT: isb
# CHECK-NEXT: mrs x16, PMCCNTR_EL0
# CHECK:      str x16, [x17]
# CHECK:      mov x1, #0x0
# CHECK:      mov x2, #0x0
# CHECK-NEXT: add x1, x1, x2
# CHECK-NEXT: add x1, x1, x2
# CHECK-NEXT: isb
# CHECK-NEXT: mrs x16, PMCCNTR_EL0
# CHECK:      str x16, [x17]
# CHECK-NEXT: ret
