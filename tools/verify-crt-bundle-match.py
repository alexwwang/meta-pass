#!/usr/bin/env python3
# tools/verify-crt-bundle-match.py — BUG-18 static reproduction (host-side, no device).
#
# Reproduces, byte-for-byte, what the ESP32 does during TLS handshake:
#   esp_crt_verify_callback() runs for EVERY cert in the presented chain (depth 0..n-1).
#   For each cert it calls esp_crt_find_cert(child->issuer_raw.p, child->issuer_raw.len):
#   binary search over the bundle (sorted by subject DER) using memcmp on
#   MIN(issuer_len, name_len) bytes — a byte-prefix match is accepted.
#
# mbedTLS issuer_raw is the RAW DER Name slice from the certificate
# (SEQUENCE header included, x509_crt.c:1174-1191). gen_crt_bundle.py stores
# cryptography's re-encoded subject.public_bytes() — which MAY differ from the
# raw bytes. This script compares both, exposing any encoding delta.
#
# Checks:
#   1. bundle structural integrity (mirrors esp_crt_check_bundle)
#   2. bundle sorted by subject DER (binary-search precondition)
#   3. gen-script name bytes == raw subject DER of main/certs PEMs
#   4. chain walk: for each depth, issuer_raw ∈ bundle? (the device algorithm)
#   5. bundle embedded verbatim in the app .bin?
#
# Exit 0 only if the exact chain the server presents would validate on device.
import glob
import os
import struct
import sys

# ---------- minimal DER ----------

def der_read_tlv(buf, off):
    """Return (tag, content_off, content_len, next_off)."""
    if off >= len(buf):
        raise ValueError("DER: out of bounds")
    tag = buf[off]
    i = off + 1
    b = buf[i]; i += 1
    if b & 0x80:
        n = b & 0x7F
        if n == 0:
            raise ValueError("DER: indefinite length unsupported")
        length = int.from_bytes(buf[i:i + n], "big")
        i += n
    else:
        length = b
    return tag, i, length, i + length

def der_children(buf, off, end):
    """Yield (tag, content_off, content_len, tlv_off, tlv_end) for children of a constructed TLV."""
    out = []
    p = off
    while p < end:
        tag, coff, clen, nxt = der_read_tlv(buf, p)
        out.append((tag, coff, clen, p, nxt))
        p = nxt
    return out

OID = {
    "2.5.4.3": "CN", "2.5.4.6": "C", "2.5.4.7": "L", "2.5.4.8": "ST",
    "2.5.4.10": "O", "2.5.4.11": "OU", "2.5.4.5": "serialNumber",
    "0.9.2342.19200300.100.1.25": "DC", "1.2.840.113549.1.9.1": "emailAddress",
}

def decode_oid(b):
    vals = [b[0] // 40, b[0] % 40]
    v = 0
    for byte in b[1:]:
        v = (v << 7) | (byte & 0x7F)
        if not byte & 0x80:
            vals.append(v); v = 0
    return ".".join(str(x) for x in vals)

STR_TAGS = {0x0C: "utf8", 0x13: "printable", 0x16: "ia5", 0x14: "teletex", 0x1E: "bmp"}

def name_to_str(buf, off, end):
    """Human-readable RDNSequence (diagnostics only; matching is byte-level)."""
    # Accept either a wrapped SEQUENCE (bundle entries) or a raw TLV stream of
    # SETs (content of a Name, i.e. what mbedTLS issuer_raw points into).
    tag, coff, clen, nxt = der_read_tlv(buf, off)
    if tag == 0x30 and nxt == end:
        off, end = coff, coff + clen
    parts = []
    for rdn_tag, r_off, r_len, _, _ in der_children(buf, off, end):  # SET
        for _, a_off, a_len, _, _ in der_children(buf, r_off, r_off + r_len):  # SEQUENCE
            kids = der_children(buf, a_off, a_off + a_len)
            oid_b = buf[kids[0][1]:kids[0][1] + kids[0][2]]
            vtag, v_off, v_len, _, _ = kids[1]
            key = OID.get(decode_oid(oid_b), decode_oid(oid_b))
            val = buf[v_off:v_off + v_len]
            if vtag in STR_TAGS:
                s = val.decode("utf-8" if vtag == 0x0C else "latin-1")
            else:
                s = "0x" + val.hex()
            parts.append("%s=%s" % (key, s))
    return ", ".join(parts)

class Cert:
    """Raw-byte view of an X.509 certificate."""
    def __init__(self, der, label):
        self.der = der
        self.label = label
        tag, cert_off, cert_len, _ = der_read_tlv(der, 0)
        assert tag == 0x30, "not a certificate SEQUENCE"
        cert_body = der[cert_off:cert_off + cert_len]
        outer = der_children(cert_body, 0, len(cert_body))
        assert outer[0][0] == 0x30, "no tbsCertificate"
        tbs = cert_body[outer[0][1]:outer[0][1] + outer[0][2]]  # descend into tbsCertificate
        kids = der_children(tbs, 0, len(tbs))
        idx = 0
        if kids and kids[idx][0] == 0xA0:  # [0] explicit version
            idx += 1
        assert kids[idx][0] == 0x02, "no serial"
        idx += 1
        assert kids[idx][0] == 0x30, "no sig alg"
        idx += 1
        # issuer: RAW slice including SEQUENCE header (mbedTLS issuer_raw semantics)
        _, i_off, i_len, i_tlv, i_end = kids[idx]
        self.issuer_raw = tbs[i_tlv:i_end]
        idx += 1
        assert kids[idx][0] == 0x30, "no validity"  # X.509: validity sits between issuer and subject
        idx += 1
        assert kids[idx][0] == 0x30, "no subject"
        _, s_off, s_len, s_tlv, s_end = kids[idx]
        self.subject_raw = tbs[s_tlv:s_end]
        self.issuer_str = name_to_str(tbs, i_off, i_off + i_len)
        self.subject_str = name_to_str(tbs, s_off, s_off + s_len)

    @classmethod
    def from_pem(cls, path):
        b = open(path, "rb").read()
        body = []
        grab = False
        for line in b.splitlines():
            if line.strip() == b"-----BEGIN CERTIFICATE-----":
                grab = True; body = []; continue
            if line.strip() == b"-----END CERTIFICATE-----":
                grab = False
                yield cls(__import__("base64").b64decode(b"".join(body)), path)
                continue
            if grab:
                body.append(line.strip())

def pem_certs(path):
    return list(Cert.from_pem(path))

# ---------- bundle ----------

def parse_bundle(path):
    b = open(path, "rb").read()
    count = struct.unpack("<I", b[:4])[0] // 4
    offsets = [struct.unpack("<I", b[4 * i:4 * i + 4])[0] for i in range(count)]
    entries = []
    for i, off in enumerate(offsets):
        name_len, key_len = struct.unpack("<HH", b[off:off + 4])
        name = b[off + 4:off + 4 + name_len]
        entries.append({"index": i, "offset": off, "name": name, "key": b[off + 4 + name_len:off + 4 + name_len + key_len]})
    return {"size": len(b), "count": count, "offsets": offsets, "entries": entries}

def bundle_check_structure(bun):
    """Mirror esp_crt_check_bundle: contiguous offsets, each cert fits."""
    ok = True
    for i in range(bun["count"] - 1):
        off = bun["offsets"][i]
        e = bun["entries"][i]
        expected = off + 4 + len(e["name"]) + len(e["key"])
        if bun["offsets"][i + 1] != expected:
            ok = False
    return ok

def bundle_check_sorted(bun):
    names = [e["name"] for e in bun["entries"]]
    return names == sorted(names)

# ---------- device lookup semantics ----------

def memcmp_prefix(a, b):
    """mbedTLS memcmp over MIN(len_a, len_b) bytes. Returns cmp_res (<0/0/>0)."""
    n = min(len(a), len(b))
    return (a[:n] > b[:n]) - (a[:n] < b[:n])

def esp_crt_find_cert(bundle, issuer):
    """Exact reproduction of esp_crt_find_cert (binary search, memcmp on prefix)."""
    start, end = 0, bundle["count"] - 1
    trail = []
    while start <= end:
        middle = (start + end) // 2
        e = bundle["entries"][middle]
        cmp_res = memcmp_prefix(issuer, e["name"])
        trail.append("idx=%d name_len=%d cmp=%d" % (middle, len(e["name"]), cmp_res))
        if cmp_res == 0:
            return e, trail
        if cmp_res < 0:
            end = middle - 1
        else:
            start = middle + 1
    return None, trail

def find_by_exact(bundle, name):
    for e in bundle["entries"]:
        if e["name"] == name:
            return e
    return None

# ---------- main ----------

def main():
    base = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    bundle_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(base, "build/esp-idf/mbedtls/x509_crt_bundle")
    certs_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(base, "main/certs")
    chain_files = sys.argv[3:] or ["/tmp/chain_1.pem", "/tmp/chain_2.pem", "/tmp/chain_3.pem"]
    # newest app bin in build/ (if any): the guard against flashing a stale image
    bins = sorted(glob.glob(os.path.join(base, "build", "meta-pass_v*.bin")),
                  key=os.path.getmtime)
    bin_path = bins[-1] if bins else None

    fails = []

    bundle = parse_bundle(bundle_path)
    print("== bundle %s: %d bytes, %d certs" % (bundle_path, bundle["size"], bundle["count"]))
    for e in bundle["entries"]:
        print("   [%d] off=%-5d name_len=%-3d key_len=%-4d %s" %
              (e["index"], e["offset"], len(e["name"]), len(e["key"]), name_to_str(e["name"], 0, len(e["name"]))))

    if not bundle_check_structure(bundle):
        fails.append("bundle offsets not contiguous (esp_crt_check_bundle would reject)")
    else:
        print("   structure: contiguous offsets OK")

    if not bundle_check_sorted(bundle):
        fails.append("bundle NOT sorted by subject DER (binary search broken)")
    else:
        print("   sorted by subject DER: OK")

    # raw DER of every cert in certs dir
    trust = []
    for fn in sorted(os.listdir(certs_dir)):
        if fn.endswith(".pem"):
            for c in pem_certs(os.path.join(certs_dir, fn)):
                trust.append((fn, c))
    print("\n== trust source %s:" % certs_dir)
    for fn, c in trust:
        print("   %s: subject=%s" % (fn, c.subject_str))

    # check 3: gen-script stored name vs RAW subject DER of the PEM
    print("\n== gen-script name bytes vs raw PEM subject DER:")
    for fn, c in trust:
        e = find_by_exact(bundle, c.subject_raw)
        if e is None:
            for e2 in bundle["entries"]:
                if name_to_str(e2["name"], 0, len(e2["name"])) == c.subject_str:
                    fails.append("%s: subject STRINGS equal but RAW DER bytes differ! "
                                 "bundle=%s... pem=%s... (cryptography re-encoded the Name)" %
                                 (fn, e2["name"][:16].hex(), c.subject_raw[:16].hex()))
                    print("   %-14s ENCODING MISMATCH (bytes differ, text same)" % fn)
                    break
            else:
                print("   %-14s NOT IN BUNDLE (subject raw=%s...)" % (fn, c.subject_raw[:16].hex()))
                fails.append("%s: raw subject not found in bundle" % fn)
        else:
            print("   %-14s byte-exact match, name_len=%d" % (fn, len(e["name"])))

    # check 4: the device algorithm, per depth
    print("\n== device lookup simulation (esp_crt_verify_callback per depth):")
    chain = []
    for p in chain_files:
        cs = pem_certs(p) if os.path.exists(p) else []
        if cs:
            chain.append(cs[0])
    if not chain:
        fails.append("no chain certs loaded from %s" % chain_files)
    ok = True
    for depth, c in enumerate(chain):
        e, trail = esp_crt_find_cert(bundle, c.issuer_raw)
        if e is None:
            ok = False
            fails.append("depth %d (%s): issuer [%s] NOT in bundle -> 'No matching trusted "
                         "root certificate found' (search: %s)" %
                         (depth, c.subject_str, c.issuer_str, " | ".join(trail)))
            print("   depth %d %-38s issuer=%-42s NOT FOUND" % (depth, c.subject_str, c.issuer_str))
        else:
            print("   depth %d %-38s issuer=%-42s FOUND (idx %d)" % (depth, c.subject_str, c.issuer_str, e["index"]))
    print("   => chain would %s on device" % ("VALIDATE" if ok else "FAIL"))

    # check 5: bundle embedded verbatim in the newest app bin
    if bin_path:
        raw = open(bundle_path, "rb").read()
        binb = open(bin_path, "rb").read()
        pos = binb.find(raw)
        print("\n== embedded bundle in %s: %s" % (os.path.basename(bin_path),
              "found at 0x%x" % pos if pos >= 0 else "NOT FOUND"))
        if pos < 0:
            fails.append("newest app bin does NOT embed this bundle (stale image — "
                         "flash would carry the old bundle): %s" % os.path.basename(bin_path))
    else:
        print("\n== embedded-bundle check skipped: no build/meta-pass_v*.bin")

    print("\n== RESULT: %s" % ("PASS" if not fails else "FAIL"))
    for f in fails:
        print("   FAIL: %s" % f)
    sys.exit(0 if not fails else 1)

if __name__ == "__main__":
    main()
