#!/usr/bin/env python3
"""Deterministic llvm-exegesis remote helper for tests (protocol v1).

It executes nothing. Memory is a sparse byte map. On `run` it fills the
results area with start=1000 and end=1000+N, where N is the number of
instructions (bytes / --insn-size) written at --function since the
previous run, or --fixed-cycles when given. --start, --down and --width
emulate other counters: a down counter stores end=start-N, and both values
are truncated to --width bits. Every request is appended to --log so a test
can FileCheck the exchanged protocol.
"""

import argparse
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--function", type=lambda s: int(s, 0), required=True)
    ap.add_argument("--insn-size", type=int, default=4)
    ap.add_argument("--fixed-cycles", type=int)
    ap.add_argument("--start", type=lambda s: int(s, 0), default=1000)
    ap.add_argument("--down", action="store_true")
    ap.add_argument("--width", type=int, default=64)
    ap.add_argument("--halt-pc", type=lambda s: int(s, 0),
                    help="report this PC instead of the stop address")
    ap.add_argument("--fail-on", help="answer 'err' to this request kind")
    ap.add_argument("--log")
    opts = ap.parse_args()

    log = open(opts.log, "a") if opts.log else None
    mem = {}
    code_bytes = 0

    def reply(line):
        sys.stdout.write(line + "\n")
        sys.stdout.flush()

    for raw in sys.stdin:
        line = raw.strip()
        if not line:
            continue
        if log:
            # Keep the log readable: abbreviate long hex payloads.
            words = line.split()
            if words[0] == "write":
                words[2] = f"<{len(words[2]) // 2} bytes>"
            log.write(" ".join(words) + "\n")
            log.flush()
        op, args = line.split()[0], line.split()[1:]
        if op == opts.fail_on:
            reply(f"err mock failure on '{op}'")
            continue
        if op == "hello":
            reply("ok 1 mock")
        elif op == "write":
            addr, data = int(args[0], 0), bytes.fromhex(args[1])
            for i, b in enumerate(data):
                mem[addr + i] = b
            if addr == opts.function:
                code_bytes += len(data)
            reply("ok")
        elif op == "read":
            addr, n = int(args[0], 0), int(args[1], 0)
            reply("ok " + bytes(mem.get(addr + i, 0) for i in range(n)).hex())
        elif op == "run":
            stop = int(args[1], 0)
            if opts.fixed_cycles is not None:
                cycles = opts.fixed_cycles
            else:
                cycles = code_bytes // opts.insn_size
            code_bytes = 0
            end = opts.start - cycles if opts.down else opts.start + cycles
            mask = (1 << opts.width) - 1
            for slot, value in enumerate((opts.start & mask, end & mask)):
                for i, b in enumerate(value.to_bytes(8, "little")):
                    mem[opts.results + 8 * slot + i] = b
            pc = opts.halt_pc if opts.halt_pc is not None else stop
            reply(f"ok halted 0x{pc:x}")
        elif op == "cmd":
            reply("ok " + b"mock".hex())
        elif op == "quit":
            reply("ok")
            break
        else:
            reply(f"err unknown request '{op}'")
    return 0


if __name__ == "__main__":
    sys.exit(main())
