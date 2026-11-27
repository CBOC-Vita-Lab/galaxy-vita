#!/usr/bin/env python3
"""Write net/src/np_https_roots.h: the root certificates the game's HTTPS client trusts.

    NP_HTTPS_ROOTS_DAY=YYYY-MM-DD python3 tools/net/gen_https_roots.py [CERT_DIR ...] [--new-isrg-roots] [--pin STEM=SHA256 ...] \
        > net/src/np_https_roots.h

Galaxy 3.0 (tls agent, Nov 11 2026): from Strikers Charged 1.3 native/tools/net/gen_https_roots.py (same owner). With
NP_HTTPS_ROOTS_DAY=2026-10-30 and the Ubuntu 24.04 ca-certificates 20260816ubuntu0.24.04.1 PEMs in /etc/ssl/certs it reproduces the
header of the 3.0 tree byte for byte (tests: docs/ONLINE-TLS.md). Additions: several CERT_DIRs (each stem is taken from the first that
has it), --new-isrg-roots (also ISRG Root YE / YR, from files ISRG_Root_YE.pem / ISRG_Root_YR.pem that a person put in one of the
CERT_DIRs after checking their fingerprints: docs/ONLINE-TLS.md, "Adding ISRG Root YE / YR"), and --pin STEM=SHA256 (refuse to write
the header unless that root's DER has that SHA-256).

The roots are the ones a Cloudflare-hosted service can chain to today (documented in docs/ONLINE-TLS.md):
  Google Trust Services (WR*/WE* intermediates):  GTS Root R1 (RSA), GTS Root R4 (ECDSA P-384)
  Let's Encrypt:                                  ISRG Root X1 (RSA), ISRG Root X2 (ECDSA P-384)
  SSL.com, behind "Cloudflare TLS Issuing" CAs:   SSL.com TLS ECC / RSA Root CA 2022, SSL.com Root Certification Authority ECC / RSA
Nothing else: no DigiCert / Baltimore (Cloudflare-managed certificates do not use them), no system store.
They are read from a PEM directory (default /etc/ssl/certs, the ca-certificates package) and embedded as DER with their
SHA-256, which the client checks again at start-up (a corrupted header fails closed). The header also records the day it was
generated: the client treats a console clock earlier than that day as "set your clock", not as a certificate fault.

Stdlib only: no OpenSSL binding is needed (PEM -> DER by base64, subject and notAfter by a small DER walk)."""
import base64
import calendar
import datetime
import hashlib
import os
import re
import sys

ROOTS = [
    # (file stem in CERT_DIR, who may serve it)
    ("GTS_Root_R1", "Google Trust Services, RSA chains (WR1-WR4)"),
    ("GTS_Root_R4", "Google Trust Services, ECDSA chains (WE1-WE4)"),
    ("ISRG_Root_X1", "Let's Encrypt, RSA chains and the cross-signed ECDSA chains"),
    ("ISRG_Root_X2", "Let's Encrypt, ECDSA chains (E5-E9)"),
    ("SSL.com_TLS_ECC_Root_CA_2022", "SSL.com via Cloudflare TLS Issuing ECC CA"),
    ("SSL.com_TLS_RSA_Root_CA_2022", "SSL.com via Cloudflare TLS Issuing RSA CA"),
    ("SSL.com_Root_Certification_Authority_ECC", "SSL.com, 2016 ECC root (Transit ECC CA R2)"),
    ("SSL.com_Root_Certification_Authority_RSA", "SSL.com, 2016 RSA root (Transit RSA CA R2)"),
]
# Let's Encrypt's new roots (2026), cross-signed by ISRG Root X2 / X1 while those cross-signs are served. Not in the header until a
# person provides the PEMs and their fingerprints are checked (docs/ONLINE-TLS.md); never typed in by hand.
NEW_ISRG_ROOTS = [
    ("ISRG_Root_YE", "Let's Encrypt, new ECDSA hierarchy (YE1...; served cross-signed by ISRG Root X2 until it is not)"),
    ("ISRG_Root_YR", "Let's Encrypt, new RSA hierarchy (served cross-signed by ISRG Root X1 until it is not)"),
]


def der_walk(b, off=0):
    """Return (tag, header_len, content_len) of the TLV at off."""
    tag = b[off]
    n = b[off + 1]
    if n < 0x80:
        return tag, 2, n
    k = n & 0x7F
    return tag, 2 + k, int.from_bytes(b[off + 2:off + 2 + k], "big")


def tbs_fields(der):
    """(subject_cn, not_after_epoch) of an X.509 certificate."""
    tag, h, n = der_walk(der, 0)
    off = h                                  # inside Certificate
    tag, h, n = der_walk(der, off)           # tbsCertificate
    off += h
    tag, h, n = der_walk(der, off)
    if tag == 0xA0:                          # [0] version
        off += h + n
    fields = []
    for _ in range(5):                       # serial, sigalg, issuer, validity, subject
        tag, h, n = der_walk(der, off)
        fields.append((tag, off, h, n))
        off += h + n
    _, voff, vh, vn = fields[3]
    t1 = der_walk(der, voff + vh)
    o2 = voff + vh + t1[1] + t1[2]
    t2 = der_walk(der, o2)
    s = der[o2 + t2[1]:o2 + t2[1] + t2[2]].decode()
    if t2[0] == 0x17:                        # UTCTime
        yy = int(s[0:2])
        year = 1900 + yy if yy >= 50 else 2000 + yy
        rest = s[2:]
    else:                                    # GeneralizedTime
        year = int(s[0:4])
        rest = s[4:]
    dt = datetime.datetime(year, int(rest[0:2]), int(rest[2:4]), int(rest[4:6]), int(rest[6:8]), int(rest[8:10]))
    not_after = calendar.timegm(dt.timetuple())
    _, soff, sh, sn = fields[4]
    subject = der[soff + sh:soff + sh + sn]
    cn = b""
    i = 0
    while i < len(subject):                  # SET { SEQ { OID, value } } ...
        _, h1, n1 = der_walk(subject, i)
        seq = subject[i + h1:i + h1 + n1]
        _, h2, n2 = der_walk(seq, 0)
        inner = seq[h2:h2 + n2]
        if inner[0] == 0x06 and inner[2:2 + inner[1]] == b"\x55\x04\x03":
            vt, vh2, vn2 = der_walk(inner, 2 + inner[1])
            cn = inner[2 + inner[1] + vh2:2 + inner[1] + vh2 + vn2]
        i += h1 + n1
    return cn.decode(), not_after


def find_pem(cert_dirs, stem):
    for d in cert_dirs:
        path = os.path.join(d, stem + ".pem")
        if os.path.isfile(path):
            return path
    sys.exit("%s.pem: not found in %s" % (stem, ", ".join(cert_dirs)))


def main():
    args = sys.argv[1:]
    roots = list(ROOTS)
    pins = {}
    cert_dirs = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--new-isrg-roots":
            roots += NEW_ISRG_ROOTS
        elif a == "--pin" and i + 1 < len(args):
            stem, _, hexsum = args[i + 1].partition("=")
            if not re.fullmatch(r"[0-9a-fA-F]{64}", hexsum):
                sys.exit("--pin %s: want STEM=<64 hex digits>" % args[i + 1])
            pins[stem] = hexsum.lower().replace(":", "")
            i += 1
        elif a.startswith("--"):
            sys.exit("unknown option " + a)
        else:
            cert_dirs.append(a)
        i += 1
    cert_dirs = cert_dirs or ["/etc/ssl/certs"]
    for stem in pins:
        if stem not in [r[0] for r in roots]:
            sys.exit("--pin %s: not a root of this list" % stem)
    day = os.environ.get("NP_HTTPS_ROOTS_DAY")      # YYYY-MM-DD, for a reproducible header
    gen = datetime.datetime.strptime(day, "%Y-%m-%d") if day else datetime.datetime.now(datetime.timezone.utc).replace(tzinfo=None)
    built_at = calendar.timegm(datetime.datetime(gen.year, gen.month, gen.day).timetuple())
    out = ["/* np_https_roots.h -- GENERATED by tools/net/gen_https_roots.py; do not edit.",
           " * The root certificates np_https trusts (and nothing else). Reasons: docs/ONLINE-TLS.md.",
           " * NP_HTTPS_ROOTS_DAY: the day this list was generated (UTC midnight); a console clock before it is a clock error. */",
           "#ifndef NP_HTTPS_ROOTS_H", "#define NP_HTTPS_ROOTS_H", "",
           "#define NP_HTTPS_ROOTS_DAY %dLL" % built_at, ""]
    names = []
    for stem, who in roots:
        pem = open(find_pem(cert_dirs, stem)).read()
        m = re.search(r"-----BEGIN CERTIFICATE-----(.*?)-----END CERTIFICATE-----", pem, re.S)
        if not m or pem.count("BEGIN CERTIFICATE") != 1:
            sys.exit(stem + ": not exactly one PEM certificate")
        der = base64.b64decode("".join(m.group(1).split()))
        cn, not_after = tbs_fields(der)
        sha = hashlib.sha256(der).digest()
        if stem in pins and sha.hex() != pins[stem]:
            sys.exit("%s: SHA-256 %s is not the pinned %s; header NOT written" % (stem, sha.hex(), pins[stem]))
        ident = "np_root_" + re.sub(r"[^A-Za-z0-9]", "_", stem)
        names.append((ident, cn, who, not_after))
        out.append("/* %s -- %s; valid until %s; sha256 %s */" % (
            cn, who, datetime.datetime.fromtimestamp(not_after, datetime.timezone.utc).strftime("%Y-%m-%d"), sha.hex()))
        out.append("static const unsigned char %s[] = {" % ident)
        for i in range(0, len(der), 16):
            out.append("    " + ",".join("0x%02x" % c for c in der[i:i + 16]) + ",")
        out.append("};")
        out.append("static const unsigned char %s_sha256[32] = {" % ident)
        out.append("    " + ",".join("0x%02x" % c for c in sha) + "};")
        out.append("")
    out.append("struct NpHttpsRootEntry { const char* name; const unsigned char* der; unsigned len; const unsigned char* sha256; long long not_after; };")
    out.append("static const NpHttpsRootEntry np_https_roots[] = {")
    for ident, cn, who, not_after in names:
        out.append('    { "%s", %s, (unsigned)sizeof(%s), %s_sha256, %dLL },' % (cn, ident, ident, ident, not_after))
    out.append("};")
    out.append("#define NP_HTTPS_ROOT_COUNT %d" % len(names))
    out.append("")
    out.append("#endif")
    sys.stdout.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
