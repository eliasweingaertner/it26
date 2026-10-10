"""Minimal synchronous QMP client (QEMU Machine Protocol over TCP)."""

from __future__ import annotations

import json
import socket
import time


class QMPError(RuntimeError):
    pass


class QMP:
    def __init__(self, host: str, port: int, timeout: float = 30.0):
        deadline = time.monotonic() + timeout
        while True:
            try:
                self.sock = socket.create_connection((host, port), timeout=5)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.sock.settimeout(timeout)
        self.buf = b""
        self.events: list[dict] = []
        self._read()                       # greeting
        self.cmd("qmp_capabilities")

    def _read(self) -> dict:
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise QMPError("QMP connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line)

    def cmd(self, name: str, **args):
        msg = {"execute": name}
        if args:
            msg["arguments"] = args
        self.sock.sendall(json.dumps(msg).encode() + b"\n")
        while True:
            r = self._read()
            if "event" in r:
                self.events.append(r)
                continue
            if "error" in r:
                raise QMPError(f"{name}: {r['error'].get('desc')}")
            return r.get("return")

    def hmp(self, line: str) -> str:
        return self.cmd("human-monitor-command", **{"command-line": line})

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass
