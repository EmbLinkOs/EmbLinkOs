#!/usr/bin/env python3
"""Run a guest DNS/HTTP client check against deterministic host fixtures.

Uses the ordinary hardware_smoke.py NIC/DHCP/RX/socket path. A test-only host
interposer redirects SLIRP's upstream DNS and HTTP sockets onto loopback. This
validates guest networking without claiming public Internet or RF connectivity.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading

root = Path(__file__).resolve().parents[1]


class Page(BaseHTTPRequestHandler):
    requests = 0

    def do_GET(self):
        type(self).requests += 1
        body = b"<!doctype html><title>EmbLinkOS network test</title><p>DNS and HTTP reached the fixture.</p>\n"
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--command", choices=["test dns", "test tcp"], default="test tcp")
    args = parser.parse_args()
    dns = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dns.bind(("127.0.0.1", 53))
    dns.settimeout(0.2)
    queries = []
    done = threading.Event()
    failures = []

    def resolver():
        try:
            while not done.is_set():
                try:
                    data, peer = dns.recvfrom(2048)
                except socket.timeout:
                    continue
                if len(data) < 17 or struct.unpack_from("!H", data, 4)[0] != 1:
                    continue
                at = 12
                labels = []
                while at < len(data) and data[at]:
                    n = data[at]
                    at += 1
                    if n > 63 or at + n >= len(data):
                        break
                    labels.append(data[at:at + n].decode("ascii"))
                    at += n
                else:
                    if at + 5 > len(data) or data[at:at + 5] != b"\0\0\1\0\1":
                        continue
                    name = ".".join(labels)
                    queries.append(name)
                    if name not in ("example.com", "dns.google"):
                        continue
                    # A compressed A/IN answer for the echoed question.
                    header = data[:2] + struct.pack("!5H", 0x8180, 1, 1, 0, 0)
                    answer = b"\xc0\x0c" + struct.pack("!HHIH", 1, 1, 60, 4) + bytes([192, 0, 2, 80])
                    dns.sendto(header + data[12:at + 5] + answer, peer)
        except Exception as error:
            failures.append(error)

    resolver_thread = threading.Thread(target=resolver)
    http = ThreadingHTTPServer(("127.0.0.1", 0), Page)
    http_thread = threading.Thread(target=http.serve_forever)
    resolver_thread.start()
    http_thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="emblink-network-fixture-") as tmp:
            lib = Path(tmp) / "redirect.so"
            subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-O2", "-Wall", "-Wextra", "-Werror",
                            str(root / "tests/drivers/network_redirect.c"), "-ldl", "-o", str(lib)], check=True)
            env = os.environ.copy()
            env["LD_PRELOAD"] = str(lib)
            env["EMBLINK_TEST_DNS_PORT"] = str(dns.getsockname()[1])
            env["EMBLINK_TEST_HTTP_PORT"] = str(http.server_port)
            env["EMBLINK_SMOKE_COMMAND"] = args.command
            env["EMBLINK_SMOKE_EXPECT"] = f"[cmd] {args.command}: OK"
            env.setdefault("EMBLINK_NIC", "e1000e")
            subprocess.run([sys.executable, str(root / "tools/hardware_smoke.py")], env=env, cwd=root, check=True)
        if failures:
            raise failures[0]
        if "example.com" not in queries or (args.command == "test tcp" and not Page.requests):
            raise RuntimeError("guest success marker without DNS/HTTP fixture traffic")
        print(f"PASS: local DNS fixture queries={queries}, HTTP requests={Page.requests}; public upstream and physical Wi-Fi untested")
    finally:
        done.set()
        resolver_thread.join()
        dns.close()
        http.shutdown()
        http_thread.join()
        http.server_close()


if __name__ == "__main__":
    main()
