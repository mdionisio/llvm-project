#!/usr/bin/env python3
"""F1 smoke test: drive QEMU aarch64 virt through the helper protocol.

Plays the role of llvm-exegesis: starts QEMU halted with its gdbstub, starts
the helper, loads a PMU setup function and a hand-written snippet into RAM,
runs them and reads the cycle counter values back from the results area.

QEMU is not cycle accurate: this only checks the plumbing.
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))

CODE = 0x40080000
RETURN_PAD = CODE
SETUP_FN = CODE + 0x100
SNIPPET_FN = CODE + 0x1000
RESULTS = 0x400FF000
SCRATCH = 0x40100000
STACK_TOP = 0x40200000

# Enable FP/SIMD at EL1 and the PMU cycle counter. x0 is unused.
SETUP_ASM = """
  mov x1, #(3 << 20)
  msr cpacr_el1, x1          // CPACR_EL1.FPEN = 0b11
  mrs x1, pmcr_el0
  orr x1, x1, #1             // E: enable
  orr x1, x1, #4             // C: reset the cycle counter
  msr pmcr_el0, x1
  mov x1, #(1 << 31)
  msr pmcntenset_el0, x1     // enable PMCCNTR_EL0
  msr pmccfiltr_el0, xzr     // count at every exception level
  isb
  ret
"""

# x0 = scratch (unused), x9 = results area. Same shape that llvm-exegesis
# will generate: counter read, body, counter read, return.
SNIPPET_ASM = """
  isb
  mrs x10, pmccntr_el0
  str x10, [x9]
  mov x2, #{iters}
1:
  subs x2, x2, #1
  b.ne 1b
  isb
  mrs x10, pmccntr_el0
  str x10, [x9, #8]
  ret
"""


def assemble(asm, workdir, name):
    src = os.path.join(workdir, name + ".s")
    obj = os.path.join(workdir, name + ".o")
    binf = os.path.join(workdir, name + ".bin")
    with open(src, "w") as f:
        f.write(asm)
    subprocess.run(
        ["clang", "--target=aarch64-none-elf", "-c", src, "-o", obj], check=True
    )
    subprocess.run(
        ["llvm-objcopy", "-O", "binary", "-j", ".text", obj, binf], check=True
    )
    with open(binf, "rb") as f:
        return f.read()


class Helper:
    def __init__(self, cmd):
        self.p = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True
        )

    def req(self, line):
        self.p.stdin.write(line + "\n")
        self.p.stdin.flush()
        reply = self.p.stdout.readline().strip()
        if not reply.startswith("ok"):
            raise RuntimeError(f"'{line[:60]}' -> {reply}")
        return reply[3:]

    def close(self):
        try:
            self.req("quit")
        finally:
            self.p.wait(5)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("--iters", type=int, default=1000)
    ap.add_argument("-v", "--verbose", action="store_true")
    opts = ap.parse_args()

    qemu = subprocess.Popen(
        [
            "qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a53",
            "-m", "512M", "-nographic", "-monitor", "none", "-serial", "none",
            "-icount", "shift=0", "-S", "-gdb", f"tcp::{opts.port}",
        ],
        stdout=subprocess.DEVNULL,
    )
    time.sleep(0.5)
    helper_cmd = [
        sys.executable,
        os.path.join(HERE, "exegesis-gdb-remote-helper.py"),
        "--port", str(opts.port),
    ] + (["-v"] if opts.verbose else [])
    try:
        with tempfile.TemporaryDirectory() as tmp:
            setup = assemble(SETUP_ASM, tmp, "setup")
            pad = assemble("b .\n", tmp, "pad")
            h = Helper(helper_cmd)
            print("hello ->", h.req("hello 1"))
            h.req(f"write 0x{RETURN_PAD:x} {pad.hex()}")
            h.req(f"write 0x{SETUP_FN:x} {setup.hex()}")
            stop = h.req(
                f"run 0x{SETUP_FN:x} 0x{RETURN_PAD:x} 2000 "
                f"sp=0x{STACK_TOP:x} x30=0x{RETURN_PAD:x}"
            )
            assert stop == f"halted 0x{RETURN_PAD:x}", stop
            print("setup  ->", stop)

            deltas = []
            for iters in (opts.iters, 2 * opts.iters):
                body = assemble(SNIPPET_ASM.format(iters=iters), tmp, "snippet")
                h.req(f"write 0x{SNIPPET_FN:x} {body.hex()}")
                h.req(f"write 0x{RESULTS:x} {'00' * 16}")
                stop = h.req(
                    f"run 0x{SNIPPET_FN:x} 0x{RETURN_PAD:x} 2000 "
                    f"sp=0x{STACK_TOP:x} x30=0x{RETURN_PAD:x} "
                    f"x0=0x{SCRATCH:x} x9=0x{RESULTS:x}"
                )
                assert stop == f"halted 0x{RETURN_PAD:x}", stop
                raw = bytes.fromhex(h.req(f"read 0x{RESULTS:x} 16"))
                start = int.from_bytes(raw[:8], "little")
                end = int.from_bytes(raw[8:], "little")
                print(f"iters={iters:6d} start={start} end={end} delta={end - start}")
                deltas.append(end - start)

            # With -icount shift=0 the QEMU cycle counter advances by one per
            # instruction: the middle-half difference must be exactly the 2
            # instructions per iteration of the added iterations.
            middle_half = deltas[1] - deltas[0]
            print(f"middle-half delta={middle_half} (expected {2 * opts.iters})")
            if middle_half != 2 * opts.iters:
                print("FAIL: unexpected middle-half delta")
                return 1

            try:
                print("monitor ->", bytes.fromhex(h.req("cmd info version")).decode().strip())
            except RuntimeError as e:
                print("monitor not available:", e)
            h.close()
    finally:
        qemu.terminate()
        qemu.wait(5)
    return 0


if __name__ == "__main__":
    sys.exit(main())
