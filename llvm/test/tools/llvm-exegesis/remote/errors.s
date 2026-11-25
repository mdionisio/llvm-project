# Errors of the remote execution mode.

# RUN: not llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/aarch64.yaml \
# RUN:   --snippets-file=%s 2>&1 | FileCheck %s --check-prefix=NO-HELPER
# NO-HELPER: needs --remote-helper and --remote-target

# RUN: not llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=uops \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/aarch64.yaml \
# RUN:   --remote-helper=unused --snippets-file=%s 2>&1 \
# RUN:   | FileCheck %s --check-prefix=UOPS
# UOPS: only measures cycles

# RUN: not llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/overlap.yaml \
# RUN:   --remote-helper=unused --snippets-file=%s 2>&1 \
# RUN:   | FileCheck %s --check-prefix=OVERLAP
# OVERLAP: the 'code' and 'scratch' regions overlap

# RUN: llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/aarch64.yaml \
# RUN:   --remote-helper='%exegesis_mock_helper --function 0x40081000 --results 0x400ff000 --halt-pc 0x1234' \
# RUN:   --snippets-file=%s | FileCheck %s --check-prefix=FAULT
# FAULT: error: {{.*}}the snippet stopped at 0x1234 instead of returning to the return pad (0x40080000)

# RUN: not llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/aarch64.yaml \
# RUN:   --remote-helper='%exegesis_mock_helper --function 0x40081000 --results 0x400ff000 --fail-on hello' \
# RUN:   --snippets-file=%s 2>&1 | FileCheck %s --check-prefix=HELLO
# HELLO: remote helper: 'hello' failed: mock failure on 'hello'

# The init steps of the description reach the helper in order.
# RUN: rm -f %t.log
# RUN: llvm-exegesis --mtriple=aarch64 --mcpu=cortex-a53 --mode=latency \
# RUN:   --execution-mode=remote --remote-target=%S/Inputs/init.yaml \
# RUN:   --remote-helper='%exegesis_mock_helper --function 0x40081000 --results 0x400ff000 --log %t.log' \
# RUN:   --snippets-file=%s > /dev/null
# RUN: FileCheck %s --check-prefix=INIT < %t.log
# INIT:      hello 1
# INIT-NEXT: cmd reset halt
# INIT-NEXT: write 0xe000edfc <4 bytes>
# INIT-NEXT: write 0x40100000 <256 bytes>

# LLVM-EXEGESIS-DEFREG X1 0
# LLVM-EXEGESIS-DEFREG X2 0
add x1, x1, x2
