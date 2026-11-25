#!/usr/bin/env python3
"""Reference llvm-exegesis remote helper speaking the GDB remote protocol.

llvm-exegesis starts this helper once and talks to it on stdin/stdout with
the line protocol described in llvm-exegesis-debug.md (protocol v1). The
helper connects to any GDB remote server: QEMU (-s), OpenOCD, the J-Link GDB
server or `pyocd gdbserver`.

Usage: exegesis-gdb-remote-helper.py [--host H] [--port P] [-v]
"""

import argparse
import re
import socket
import sys
import time

PROTOCOL_VERSION = 1


def log(msg):
    print(f"[gdb-helper] {msg}", file=sys.stderr, flush=True)


class RSPError(Exception):
    pass


class RSPClient:
    def __init__(self, host, port, verbose=False):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.verbose = verbose
        self.buf = b""
        self.packet_size = 4096
        self.reg_numbers = {}  # name -> (regnum, bitsize)
        self.no_ack = False

    # --- framing --------------------------------------------------------
    def _recv_byte(self, timeout):
        while not self.buf:
            self.sock.settimeout(timeout)
            data = self.sock.recv(65536)
            if not data:
                raise RSPError("connection closed by the GDB server")
            self.buf += data
        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def send_packet(self, payload):
        data = payload.encode("latin-1")
        csum = sum(data) & 0xFF
        frame = b"$" + data + b"#" + f"{csum:02x}".encode()
        if self.verbose:
            log(f"-> {payload[:120]}")
        while True:
            self.sock.sendall(frame)
            if self.no_ack:
                return
            ack = self._recv_byte(10)
            if ack == b"+":
                return
            if ack != b"-":
                # Unexpected byte (e.g. async output); put it back.
                self.buf = ack + self.buf
                return

    def recv_packet(self, timeout=10.0):
        while True:
            b = self._recv_byte(timeout)
            if b == b"$":
                break
        data = bytearray()
        while True:
            b = self._recv_byte(timeout)
            if b == b"#":
                break
            data += b
        self._recv_byte(timeout)
        self._recv_byte(timeout)  # checksum, trusted on TCP
        if not self.no_ack:
            self.sock.sendall(b"+")
        payload = self._decode_rle(bytes(data)).decode("latin-1")
        if self.verbose:
            log(f"<- {payload[:120]}")
        return payload

    @staticmethod
    def _decode_rle(data):
        out = bytearray()
        i = 0
        while i < len(data):
            c = data[i]
            if c == ord("}"):
                out.append(data[i + 1] ^ 0x20)
                i += 2
            elif c == ord("*"):
                out += bytes([out[-1]]) * (data[i + 1] - 29)
                i += 2
            else:
                out.append(c)
                i += 1
        return bytes(out)

    def command(self, payload, timeout=10.0):
        self.send_packet(payload)
        return self.recv_packet(timeout)

    # --- setup ----------------------------------------------------------
    def handshake(self):
        reply = self.command("qSupported:xmlRegisters=aarch64,arm;swbreak+;hwbreak+")
        m = re.search(r"PacketSize=([0-9a-fA-F]+)", reply)
        if m:
            self.packet_size = int(m.group(1), 16)
        if "QStartNoAckMode+" in reply and self.command("QStartNoAckMode") == "OK":
            self.no_ack = True
        if "qXfer:features:read+" in reply:
            self._load_target_xml()
        self.command("?")

    def _read_xfer(self, annex):
        out, offset = "", 0
        while True:
            reply = self.command(f"qXfer:features:read:{annex}:{offset:x},{0xffb:x}")
            if not reply or reply[0] not in "ml":
                raise RSPError(f"qXfer {annex} failed: {reply}")
            out += reply[1:]
            offset += len(reply) - 1
            if reply[0] == "l":
                return out

    def _load_target_xml(self):
        regnum = 0

        # Servers do not always emit well-formed namespaced XML (QEMU uses
        # an undeclared `xi:` prefix), so scan the tags in document order.
        tag_re = re.compile(r"<\s*(xi:include|include|reg)\b([^>]*)>")
        attr_re = re.compile(r'([\w:-]+)\s*=\s*"([^"]*)"')

        def walk(annex):
            nonlocal regnum
            for tag, attrs in tag_re.findall(self._read_xfer(annex)):
                a = dict(attr_re.findall(attrs))
                if tag.endswith("include"):
                    walk(a["href"])
                    continue
                if "regnum" in a:
                    regnum = int(a["regnum"], 0)
                self.reg_numbers[a["name"].lower()] = (regnum, int(a["bitsize"]))
                regnum += 1

        walk("target.xml")
        aliases = {"lr": ["x30", "r14"], "x30": ["lr"], "fp": ["x29"], "sp": ["r13"]}
        for name, alts in aliases.items():
            if name not in self.reg_numbers:
                for alt in alts:
                    if alt in self.reg_numbers:
                        self.reg_numbers[name] = self.reg_numbers[alt]
                        break
        log(f"{len(self.reg_numbers)} registers from target.xml")

    # --- operations -----------------------------------------------------
    def _chunk(self):
        # Hex doubles the size; keep room for the header.
        return max(16, (self.packet_size - 32) // 2)

    def write_mem(self, addr, data):
        step = self._chunk()
        for off in range(0, len(data), step):
            part = data[off : off + step]
            reply = self.command(f"M{addr + off:x},{len(part):x}:{part.hex()}")
            if reply != "OK":
                raise RSPError(f"memory write at 0x{addr + off:x} failed: {reply}")

    def read_mem(self, addr, length):
        step = self._chunk()
        out = bytearray()
        for off in range(0, length, step):
            n = min(step, length - off)
            reply = self.command(f"m{addr + off:x},{n:x}")
            if reply.startswith("E") and len(reply) == 3:
                raise RSPError(f"memory read at 0x{addr + off:x} failed: {reply}")
            out += bytes.fromhex(reply)
        return bytes(out)

    def _reg(self, name):
        try:
            return self.reg_numbers[name.lower()]
        except KeyError:
            raise RSPError(f"unknown register '{name}'")

    def write_reg(self, name, value):
        num, bits = self._reg(name)
        data = (value & ((1 << bits) - 1)).to_bytes(bits // 8, "little")
        reply = self.command(f"P{num:x}={data.hex()}")
        if reply != "OK":
            raise RSPError(f"write of register {name} failed: {reply}")

    def read_reg(self, name):
        num, _ = self._reg(name)
        reply = self.command(f"p{num:x}")
        if reply.startswith("E") and len(reply) == 3:
            raise RSPError(f"read of register {name} failed: {reply}")
        return int.from_bytes(bytes.fromhex(reply), "little")

    def insert_breakpoint(self, addr):
        # Prefer a hardware breakpoint: it works for code in any memory and
        # does not rewrite the return pad. Fall back to a software one.
        self.bp_kind = None
        for kind in ("1", "0"):
            if self.command(f"Z{kind},{addr:x},4") == "OK":
                self.bp_kind = kind
                return
        raise RSPError(f"cannot set a breakpoint at 0x{addr:x}")

    def remove_breakpoint(self, addr):
        if self.bp_kind is not None:
            self.command(f"z{self.bp_kind},{addr:x},4")
            self.bp_kind = None

    def cont(self, timeout_s):
        self.send_packet("c")
        try:
            reply = self._wait_stop(timeout_s)
        except socket.timeout:
            self.sock.sendall(b"\x03")
            self._wait_stop(10)
            raise RSPError("timeout")
        return reply

    def _wait_stop(self, timeout_s):
        deadline = time.monotonic() + timeout_s
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise socket.timeout()
            reply = self.recv_packet(remaining)
            if reply.startswith("O") and reply != "OK":
                log("target: " + bytes.fromhex(reply[1:]).decode(errors="replace"))
                continue
            if reply[:1] in ("T", "S"):
                return reply
            if reply[:1] in ("W", "X"):
                raise RSPError(f"target exited: {reply}")

    def monitor(self, text):
        self.send_packet("qRcmd," + text.encode().hex())
        out = ""
        while True:
            reply = self.recv_packet(30)
            if reply == "OK":
                return out
            if reply.startswith("O") and reply != "OK":
                out += bytes.fromhex(reply[1:]).decode(errors="replace")
                continue
            if reply == "":
                raise RSPError("monitor commands not supported")
            if reply.startswith("E"):
                raise RSPError(f"monitor command failed: {reply}")
            out += bytes.fromhex(reply).decode(errors="replace")


def parse_int(s):
    return int(s, 0)


def serve(rsp, inp, out):
    def reply(line):
        out.write(line + "\n")
        out.flush()

    for raw in inp:
        line = raw.strip()
        if not line:
            continue
        words = line.split()
        op, args = words[0], words[1:]
        try:
            if op == "hello":
                if parse_int(args[0]) != PROTOCOL_VERSION:
                    reply(f"err unsupported protocol version {args[0]}")
                    continue
                reply(f"ok {PROTOCOL_VERSION} gdb-remote")
            elif op == "write":
                rsp.write_mem(parse_int(args[0]), bytes.fromhex(args[1]))
                reply("ok")
            elif op == "read":
                reply("ok " + rsp.read_mem(parse_int(args[0]), parse_int(args[1])).hex())
            elif op == "run":
                pc, stop = parse_int(args[0]), parse_int(args[1])
                timeout_ms = parse_int(args[2])
                for assignment in args[3:]:
                    name, value = assignment.split("=", 1)
                    rsp.write_reg(name, parse_int(value))
                rsp.write_reg("pc", pc)
                rsp.insert_breakpoint(stop)
                try:
                    rsp.cont(timeout_ms / 1000.0)
                finally:
                    rsp.remove_breakpoint(stop)
                reply(f"ok halted 0x{rsp.read_reg('pc'):x}")
            elif op == "cmd":
                text = line.split(None, 1)[1] if len(words) > 1 else ""
                reply("ok " + rsp.monitor(text).encode().hex())
            elif op == "quit":
                reply("ok")
                return
            else:
                reply(f"err unknown request '{op}'")
        except (RSPError, ValueError, IndexError) as e:
            reply(f"err {e}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=1234)
    ap.add_argument("-v", "--verbose", action="store_true")
    opts = ap.parse_args()
    try:
        rsp = RSPClient(opts.host, opts.port, opts.verbose)
        rsp.handshake()
    except (OSError, RSPError) as e:
        log(f"cannot connect to {opts.host}:{opts.port}: {e}")
        return 1
    serve(rsp, sys.stdin, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
