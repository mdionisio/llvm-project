# Check the remote execution mode end to end against the mock helper. The mock
# reports one cycle per instruction of the loaded function, so the
# middle-half repetition mode must measure exactly one cycle per instruction.

# RUN: rm -f %t.log
# RUN: llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/aarch64.yaml \
# RUN:   --remote-helper='%exegesis_mock_helper --function 0x40081000 --results 0x400ff000 --log %t.log' \
# RUN:   --repetition-mode=middle-half-duplicate --min-instructions=1000 \
# RUN:   --snippets-file=%s | FileCheck %s
# RUN: FileCheck %s --check-prefix=LOG < %t.log

# LLVM-EXEGESIS-DEFREG X1 0
# LLVM-EXEGESIS-DEFREG X2 0
add x1, x1, x2

# CHECK:      mode: latency
# CHECK:      llvm_triple: aarch64
# CHECK:      measurements:
# CHECK-NEXT:   - { key: latency, value: 1, per_snippet_value: 1
# CHECK:      error: ''

# The session: handshake, scratch cleared once, return pad ("b .").
# LOG:      hello 1
# LOG-NEXT: write 0x40100000 <65536 bytes>
# LOG-NEXT: write 0x40080000 <4 bytes>
# Each run: function, cleared results, run with the ABI registers, results.
# LOG-NEXT: write 0x40081000 <{{[0-9]+}} bytes>
# LOG-NEXT: write 0x400ff000 <16 bytes>
# LOG-NEXT: run 0x40081000 0x40080000 2000 x0=0x40100000 sp=0x40200000 x30=0x40080000
# LOG-NEXT: read 0x400ff000 0x10
# LOG:      quit
