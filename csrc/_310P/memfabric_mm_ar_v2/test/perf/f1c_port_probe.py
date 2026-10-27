#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""F1-C 监听端口探针：解析 /proc/net/tcp{,6} 代替宿主缺失的 ss/netstat。

输出所有 LISTEN 套接字（地址:端口 + 尽力而为的持有进程），并在
F1C_REQUIRED_FREE_PORT（默认 8581）已被监听时以非零退出（fail-closed）。
"""
from __future__ import annotations

import os
import sys

REQUIRED_FREE_PORT = int(os.environ.get("F1C_REQUIRED_FREE_PORT", "8581"))


def _pid_by_inode() -> dict[str, str]:
    owners: dict[str, str] = {}
    for entry in os.listdir("/proc"):
        if not entry.isdigit():
            continue
        fd_dir = f"/proc/{entry}/fd"
        try:
            for fd in os.listdir(fd_dir):
                try:
                    target = os.readlink(f"{fd_dir}/{fd}")
                except OSError:
                    continue
                if target.startswith("socket:["):
                    owners.setdefault(target[8:-1], entry)
        except OSError:
            continue
    return owners


def main() -> int:
    owners = _pid_by_inode()
    listeners: list[tuple[str, int, str, str]] = []
    for path in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(path) as fh:
                lines = fh.readlines()[1:]
        except OSError:
            continue
        for line in lines:
            parts = line.split()
            if len(parts) < 10 or parts[3] != "0A":
                continue
            host_hex, port_hex = parts[1].split(":")
            port = int(port_hex, 16)
            inode = parts[9]
            listeners.append((path, port, host_hex, owners.get(inode, "-")))
    print(f"probe=/proc/net/tcp+/proc/net/tcp6 required_free_port={REQUIRED_FREE_PORT}")
    if not listeners:
        print("listeners: NONE")
    for path, port, host_hex, pid in sorted(listeners, key=lambda z: (z[1], z[0])):
        print(f"LISTEN {host_hex}:{port} pid={pid} src={path}")
    busy = [z for z in listeners if z[1] == REQUIRED_FREE_PORT]
    if busy:
        print(f"BLOCKED: port {REQUIRED_FREE_PORT} already listening", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
