#!/usr/bin/env python3
"""A local HTTPS server for the tests of platform/sdk/net/np_https (tools/https/run_https_test.sh).

    https_server.py DIR          makes DIR/pki/* (a private test CA, never committed) and serves; writes DIR/ports.json

One listening socket per scenario, all on 127.0.0.1 with random ports (names in ports.json):
  good      EC P-256 leaf for localhost, signed by test root A; TLS 1.2 and 1.3
  tls12     the same, limited to TLS 1.2        tls13   the same, limited to TLS 1.3
  rsa       RSA chain: root R -> intermediate -> leaf (the server sends leaf + intermediate)
  wronghost leaf for other.test (valid, trusted)
  expired   leaf that ended yesterday           notyet  leaf that starts tomorrow
  untrusted leaf for localhost from root B, which the client does not trust
  plain     TCP that answers a TLS hello with HTTP text   blackhole  accepts and never speaks
Paths on the TLS ports: /hello /echo /chunked /close /closerude /huge?n= /hugechunked?n= /exact?n= /slow?ms= /drip /status?code= /redirect
/gzip /badlen /truncated /stale /flaky /count /big404 -- each is one behaviour the client must handle (see tools/https/run_https_test.sh)."""
import datetime
import hashlib
import ipaddress
import json
import os
import socket
import ssl
import sys
import threading
import time
from urllib.parse import parse_qs, urlparse

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

NOW = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
DAY = datetime.timedelta(days=1)


def name(cn):
    return x509.Name([x509.NameAttribute(NameOID.ORGANIZATION_NAME, "np_https test"), x509.NameAttribute(NameOID.COMMON_NAME, cn)])


def make(cn, key, issuer_cn, issuer_key, ca=False, pathlen=None, san=None, nb=None, na=None):
    nb = nb or NOW - 30 * DAY
    na = na or NOW + 3650 * DAY
    b = (x509.CertificateBuilder().subject_name(name(cn)).issuer_name(name(issuer_cn)).public_key(key.public_key())
         .serial_number(x509.random_serial_number()).not_valid_before(nb).not_valid_after(na))
    b = b.add_extension(x509.BasicConstraints(ca=ca, path_length=pathlen), critical=True)
    if ca:
        b = b.add_extension(x509.KeyUsage(False, False, False, False, False, True, True, False, False), critical=True)
    else:
        b = b.add_extension(x509.KeyUsage(True, False, False, False, False, False, False, False, False), critical=True)
        b = b.add_extension(x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False)
    if san:
        b = b.add_extension(x509.SubjectAlternativeName([x509.DNSName(s) if not s[0].isdigit() else x509.IPAddress(ipaddress.ip_address(s)) for s in san]), critical=False)
    return b.sign(issuer_key, hashes.SHA256())


def pem_cert(c):
    return c.public_bytes(serialization.Encoding.PEM)


def pem_key(k):
    return k.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())


def build_pki(d):
    os.makedirs(d, exist_ok=True)
    ecp = lambda: ec.generate_private_key(ec.SECP256R1())
    # root A (trusted by the tests), root R (RSA), root B (never trusted)
    ka = ec.generate_private_key(ec.SECP384R1())
    ra = make("test root A", ka, "test root A", ka, ca=True, pathlen=1)
    kr = rsa.generate_private_key(65537, 3072)
    rr = make("test root R", kr, "test root R", kr, ca=True, pathlen=1)
    kb = ec.generate_private_key(ec.SECP384R1())
    rb = make("test root B", kb, "test root B", kb, ca=True, pathlen=0)
    out = {}

    def leaf(tag, cn, san, issuer_cn, issuer_key, key=None, nb=None, na=None, chain=()):
        k = key or ecp()
        c = make(cn, k, issuer_cn, issuer_key, san=san, nb=nb, na=na)
        with open(os.path.join(d, tag + ".pem"), "wb") as f:
            f.write(pem_cert(c) + b"".join(pem_cert(x) for x in chain))
        with open(os.path.join(d, tag + ".key"), "wb") as f:
            f.write(pem_key(k))
        out[tag] = (os.path.join(d, tag + ".pem"), os.path.join(d, tag + ".key"))

    leaf("good", "localhost", ["localhost", "127.0.0.1"], "test root A", ka)
    leaf("wronghost", "other.test", ["other.test"], "test root A", ka)
    leaf("expired", "localhost", ["localhost"], "test root A", ka, nb=NOW - 60 * DAY, na=NOW - 1 * DAY)
    leaf("notyet", "localhost", ["localhost"], "test root A", ka, nb=NOW + 1 * DAY, na=NOW + 60 * DAY)
    leaf("untrusted", "localhost", ["localhost"], "test root B", kb)
    ki = rsa.generate_private_key(65537, 2048)
    ri = make("test intermediate R", ki, "test root R", kr, ca=True, pathlen=0)
    leaf("rsa", "localhost", ["localhost"], "test intermediate R", ki, key=rsa.generate_private_key(65537, 2048), chain=(ri,))
    with open(os.path.join(d, "trusted.pem"), "wb") as f:
        f.write(pem_cert(ra) + pem_cert(rr))
    with open(os.path.join(d, "rootB.pem"), "wb") as f:
        f.write(pem_cert(rb))
    return out


class Scenario:
    def __init__(self, name, certfile=None, keyfile=None, minv=None, maxv=None):
        self.name = name
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(32)
        self.port = self.sock.getsockname()[1]
        self.accepted = 0
        self.ctx = None
        if certfile:
            self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            self.ctx.load_cert_chain(certfile, keyfile)
            if minv:
                self.ctx.minimum_version = minv
            if maxv:
                self.ctx.maximum_version = maxv


def read_head(conn):
    buf = b""
    while b"\r\n\r\n" not in buf:
        d = conn.recv(4096)
        if not d:
            return None, b""
        buf += d
    head, _, rest = buf.partition(b"\r\n\r\n")
    return head.decode("latin1"), rest


def send(conn, status, headers, body=b"", reason="OK"):
    h = "HTTP/1.1 %d %s\r\n" % (status, reason) + "".join("%s: %s\r\n" % kv for kv in headers) + "\r\n"
    conn.sendall(h.encode("latin1") + body)


def serve_tls(sc, conn):
    try:
        conn.settimeout(20)
        n_req = 0
        while True:
            head, rest = read_head(conn)
            n_req += 1
            if head is None:
                return
            lines = head.split("\r\n")
            method, target, _ = lines[0].split(" ", 2)
            hdr = {}
            for l in lines[1:]:
                k, _, v = l.partition(":")
                hdr[k.strip().lower()] = v.strip()
            n = int(hdr.get("content-length", "0"))
            body = rest
            while len(body) < n:
                d = conn.recv(65536)
                if not d:
                    return
                body += d
            u = urlparse(target)
            q = {k: v[0] for k, v in parse_qs(u.query).items()}
            wants_close = hdr.get("connection", "").lower() == "close"
            conn_h = ("Connection", "close" if wants_close else "keep-alive")
            p = u.path
            if p == "/hello":
                b = b"hello from the test server\n"
                send(conn, 200, [("Content-Type", "text/plain"), ("Content-Length", str(len(b))), conn_h], b)
            elif p == "/echo":
                b = json.dumps({"method": method, "path": target, "host": hdr.get("host"), "ctype": hdr.get("content-type"), "len": len(body),
                                "sha256": hashlib.sha256(body).hexdigest(), "x_test": hdr.get("x-test"), "accept_enc": hdr.get("accept-encoding"),
                                "ua": hdr.get("user-agent"), "connection": hdr.get("connection")}).encode()
                send(conn, 200, [("Content-Type", "application/json"), ("Content-Length", str(len(b))), conn_h], b)
            elif p == "/chunked":
                parts = [b"Hello, ", b"chunked ", b"world! " * 40, b"end"]
                out = b""
                for i, c in enumerate(parts):
                    out += (("%x;ext=%d\r\n" % (len(c), i)).encode() if i % 2 else ("%X\r\n" % len(c)).encode()) + c + b"\r\n"
                out += b"0\r\nX-Trailer: yes\r\nX-Other: 1\r\n\r\n"
                send(conn, 200, [("Content-Type", "text/plain"), ("Transfer-Encoding", "chunked"), conn_h], out)
            elif p in ("/close", "/closerude"):
                # an answer ended by the close of the connection: /close ends the TLS session properly (close_notify),
                # /closerude drops the socket (a truncation looks the same, so the client must refuse it)
                send(conn, 200, [("Content-Type", "text/plain"), ("Connection", "close")], b"until the connection closes\n")
                if p == "/close":
                    try:
                        conn = conn.unwrap()
                    except (ssl.SSLError, OSError, ValueError):
                        pass
                return
            elif p in ("/huge", "/exact"):
                total = int(q.get("n", "2097152"))
                send(conn, 200, [("Content-Length", str(total)), conn_h])
                sent = 0
                blk = b"x" * 65536
                while sent < total:
                    c = blk[: min(len(blk), total - sent)]
                    conn.sendall(c)
                    sent += len(c)
            elif p == "/hugechunked":
                total = int(q.get("n", "2097152"))
                send(conn, 200, [("Transfer-Encoding", "chunked"), conn_h])
                sent = 0
                while sent < total:
                    c = b"y" * min(8192, total - sent)
                    conn.sendall(b"%x\r\n" % len(c) + c + b"\r\n")
                    sent += len(c)
                conn.sendall(b"0\r\n\r\n")
            elif p == "/slow":
                time.sleep(int(q.get("ms", "5000")) / 1000)
                b = b"slow hello\n"
                send(conn, 200, [("Content-Length", str(len(b))), conn_h], b)
            elif p == "/drip":
                send(conn, 200, [("Content-Length", "1000"), conn_h])
                for _ in range(100):
                    conn.sendall(b"d")
                    time.sleep(0.2)
            elif p == "/status":
                code = int(q.get("code", "404"))
                b = json.dumps({"error": "nope", "code": code}).encode()
                send(conn, code, [("Content-Type", "application/json"), ("Content-Length", str(len(b))), conn_h], b, reason="Status")
            elif p == "/redirect":
                send(conn, 302, [("Location", "/hello"), ("Content-Length", "0"), conn_h], reason="Found")
            elif p == "/gzip":
                b = b"\x1f\x8b not really gzip"
                send(conn, 200, [("Content-Encoding", "gzip"), ("Content-Length", str(len(b))), conn_h], b)
            elif p == "/badlen":
                conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!")
            elif p == "/truncated":
                send(conn, 200, [("Content-Length", "100"), conn_h], b"only ten b")
                return
            elif p == "/stale":
                b = b"served, then the server drops the idle connection\n"
                send(conn, 200, [("Content-Length", str(len(b))), conn_h], b)
                time.sleep(0.3)
                return
            elif p == "/flaky":
                if n_req > 1:
                    return   # the second request on a connection: close without a word (a kept-alive connection that died)
                b = b"first request on this connection\n"
                send(conn, 200, [("Content-Length", str(len(b))), conn_h], b)
            elif p == "/count":
                b = str(sc.accepted).encode()
                send(conn, 200, [("Content-Length", str(len(b))), conn_h], b)
            elif p == "/big404":
                b = b"e" * 5000
                send(conn, 404, [("Content-Length", str(len(b))), conn_h], b, reason="Not Found")
            else:
                send(conn, 404, [("Content-Length", "0"), conn_h], reason="Not Found")
            if wants_close:
                return
    except (ssl.SSLError, OSError, ValueError, BrokenPipeError):
        return
    finally:
        try:
            conn.close()
        except OSError:
            pass


def accept_loop(sc):
    while True:
        try:
            raw, _ = sc.sock.accept()
        except OSError:
            return
        sc.accepted += 1
        if sc.name == "blackhole":
            threading.Thread(target=lambda r=raw: (time.sleep(60), r.close()), daemon=True).start()
            continue
        if sc.name == "plain":
            try:
                raw.recv(16)
                raw.sendall(b"HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n")
            except OSError:
                pass
            raw.close()
            continue

        def handshake(r=raw):
            try:
                r.settimeout(10)
                c = sc.ctx.wrap_socket(r, server_side=True)
            except (ssl.SSLError, OSError):
                try:
                    r.close()
                except OSError:
                    pass
                return
            serve_tls(sc, c)

        threading.Thread(target=handshake, daemon=True).start()


def main():
    d = sys.argv[1]
    certs = build_pki(os.path.join(d, "pki"))
    TL = ssl.TLSVersion
    scen = [Scenario("good", *certs["good"]), Scenario("tls12", *certs["good"], maxv=TL.TLSv1_2), Scenario("tls13", *certs["good"], minv=TL.TLSv1_3),
            Scenario("rsa", *certs["rsa"]), Scenario("wronghost", *certs["wronghost"]), Scenario("expired", *certs["expired"]),
            Scenario("notyet", *certs["notyet"]), Scenario("untrusted", *certs["untrusted"]), Scenario("plain"), Scenario("blackhole")]
    dead = socket.socket()
    dead.bind(("127.0.0.1", 0))
    dead_port = dead.getsockname()[1]
    dead.close()   # a port nobody listens on
    for sc in scen:
        threading.Thread(target=accept_loop, args=(sc,), daemon=True).start()
    ports = {sc.name: sc.port for sc in scen}
    ports["closed"] = dead_port
    tmp = os.path.join(d, "ports.json.tmp")
    with open(tmp, "w") as f:
        json.dump(ports, f)
    os.rename(tmp, os.path.join(d, "ports.json"))
    while True:
        time.sleep(3600)


if __name__ == "__main__":
    main()
