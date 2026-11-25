#!/usr/bin/env python3
"""
Build and APK-v2-sign Neo9UdfpsOverlay.apk using aapt + openssl (zero Java/JDK dependency).
Implements full Android APK Signature Scheme v2 (ID 0x7109871a, RSA-PKCS1-v1_5 SHA-256 0x0103)
with 4-byte aligned uncompressed resources.arsc as required by Android 11-17 RRO.
"""
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

KIT_DIR = os.path.dirname(os.path.abspath(__file__))
FRAMEWORK_RES = sys.argv[1] if len(sys.argv) > 1 else "/system/framework/framework-res.apk"
OUT_APK = sys.argv[2] if len(sys.argv) > 2 else os.path.join(KIT_DIR, "Neo9UdfpsOverlay.apk")
KEY_PEM = os.path.join(KIT_DIR, "neo9_overlay_key.pem")
CERT_DER = os.path.join(KIT_DIR, "neo9_overlay_cert.der")


def ensure_key_and_cert():
    if os.path.exists(KEY_PEM) and os.path.exists(CERT_DER):
        return
    subprocess.check_call([
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", KEY_PEM, "-outform", "DER", "-out", CERT_DER,
        "-days", "10000", "-subj", "/CN=Neo9GsiKitOverlay/O=Neo9GsiKit"
    ], stderr=subprocess.DEVNULL)


def zipalign_apk(src_apk: str, dst_apk: str):
    """Rewrite ZIP with 4-byte aligned uncompressed resources.arsc and clean local/CD headers."""
    entries = []
    with zipfile.ZipFile(src_apk, "r") as zin:
        for info in zin.infolist():
            data = zin.read(info.filename)
            compress = zipfile.ZIP_STORED if info.filename == "resources.arsc" else zipfile.ZIP_DEFLATED
            entries.append((info.filename, data, compress))

    out = bytearray()
    cd_records = []
    for fname, raw_data, compress in entries:
        fn_bytes = fname.encode("utf-8")
        crc = zipfile.crc32(raw_data) & 0xffffffff
        if compress == zipfile.ZIP_DEFLATED:
            import zlib
            cobj = zlib.compressobj(9, zlib.DEFLATED, -15)
            comp_data = cobj.compress(raw_data) + cobj.flush()
            method = 8
        else:
            comp_data = raw_data
            method = 0

        # Compute padding in extra field for 4-byte alignment of uncompressed data
        extra = b""
        if method == 0:
            data_pos = len(out) + 30 + len(fn_bytes)
            pad = (4 - (data_pos % 4)) % 4
            extra = b"\x00" * pad

        lh_off = len(out)
        out += struct.pack(
            "<IHHHHHIIIHH",
            0x04034b50, 20, 0, method, 0, 0, crc, len(comp_data), len(raw_data),
            len(fn_bytes), len(extra)
        )
        out += fn_bytes + extra + comp_data

        cd_records.append(struct.pack(
            "<IHHHHHHIIIHHHHHII",
            0x02014b50, 20, 20, 0, method, 0, 0, crc, len(comp_data), len(raw_data),
            len(fn_bytes), 0, 0, 0, 0, 0, lh_off
        ) + fn_bytes)

    cd_off = len(out)
    cd = b"".join(cd_records)
    out += cd
    out += struct.pack(
        "<IHHHHIIH",
        0x06054b50, 0, 0, len(entries), len(entries), len(cd), cd_off, 0
    )
    with open(dst_apk, "wb") as f:
        f.write(out)


def lp(b: bytes) -> bytes:
    """uint32 length-prefixed sequence."""
    return struct.pack("<I", len(b)) + b


def chunk_digests(buf: bytes) -> list[bytes]:
    chunks = []
    chunk_sz = 1024 * 1024
    if len(buf) == 0:
        chunks.append(hashlib.sha256(b"\xa5\x00\x00\x00\x00").digest())
        return chunks
    for off in range(0, len(buf), chunk_sz):
        piece = buf[off : off + chunk_sz]
        h = hashlib.sha256()
        h.update(b"\xa5")
        h.update(struct.pack("<I", len(piece)))
        h.update(piece)
        chunks.append(h.digest())
    return chunks


def sign_apk_v2(in_apk: str, out_apk: str):
    data = open(in_apk, "rb").read()
    eocd_off = data.rfind(b"\x50\x4b\x05\x06")
    if eocd_off < 0:
        raise RuntimeError("EOCD not found")
    _, _, _, _, _, cd_sz, cd_off, _ = struct.unpack_from("<IHHHHIIH", data, eocd_off)

    section1 = data[:cd_off]
    section3 = data[cd_off:eocd_off]
    eocd_orig = bytearray(data[eocd_off:])
    # Per APK Sig v2 spec, EOCD central_dir_offset is set to section1 len (= cd_off) during digest
    struct.pack_into("<I", eocd_orig, 16, cd_off)
    section4 = bytes(eocd_orig)

    # Top-level SHA-256 digest over chunked sections 1, 3, 4
    all_chunks = chunk_digests(section1) + chunk_digests(section3) + chunk_digests(section4)
    top = hashlib.sha256()
    top.update(b"\x5a")
    top.update(struct.pack("<I", len(all_chunks)))
    for c in all_chunks:
        top.update(c)
    content_digest = top.digest()

    cert_der = open(CERT_DER, "rb").read()
    pub_der = subprocess.check_output(
        ["openssl", "pkey", "-in", KEY_PEM, "-pubout", "-outform", "DER"],
        stderr=subprocess.DEVNULL,
    )

    sig_alg = 0x0103  # RSASSA-PKCS1-v1_5 with SHA-256
    digests_seq = lp(struct.pack("<I", sig_alg) + lp(content_digest))
    certs_seq = lp(cert_der)
    attrs_seq = b""
    signed_data = lp(digests_seq) + lp(certs_seq) + lp(attrs_seq)

    # Sign signed_data with RSA-PKCS1-v1_5 SHA-256
    with tempfile.NamedTemporaryFile(delete=False) as tf:
        tf.write(signed_data)
        sd_path = tf.name
    try:
        sig_bytes = subprocess.check_output(
            ["openssl", "dgst", "-sha256", "-sign", KEY_PEM, sd_path],
            stderr=subprocess.DEVNULL,
        )
    finally:
        os.unlink(sd_path)

    sigs_seq = lp(struct.pack("<I", sig_alg) + lp(sig_bytes))
    signer = lp(signed_data) + lp(sigs_seq) + lp(pub_der)
    v2_value = lp(lp(signer))

    # Build APK Signing Block (ID = 0x7109871a), 4096-byte page-padded optional, here exact spec
    pair_body = struct.pack("<I", 0x7109871a) + v2_value
    pairs = struct.pack("<Q", len(pair_body)) + pair_body

    # Pad signing block with verity padding (0x42726577) so total block size is multiple of 4096?
    # Standard APK v2 requires: [size_of_block u64][pairs...][size_of_block u64][magic 16B]
    # Total block size = 8 + len(pairs) + 8 + 16; size_of_block = len(pairs) + 8 + 16
    size_of_block = len(pairs) + 24
    sig_block = (
        struct.pack("<Q", size_of_block)
        + pairs
        + struct.pack("<Q", size_of_block)
        + b"APK Sig Block 42"
    )

    new_cd_off = len(section1) + len(sig_block)
    new_eocd = bytearray(data[eocd_off:])
    struct.pack_into("<I", new_eocd, 16, new_cd_off)

    with open(out_apk, "wb") as f:
        f.write(section1)
        f.write(sig_block)
        f.write(section3)
        f.write(new_eocd)

    # Local cryptographic self-verification
    with tempfile.NamedTemporaryFile(delete=False) as pf, \
         tempfile.NamedTemporaryFile(delete=False) as sf, \
         tempfile.NamedTemporaryFile(delete=False) as df:
        pf.write(pub_der); ppath = pf.name
        sf.write(sig_bytes); spath = sf.name
        df.write(signed_data); dpath = df.name
    try:
        subprocess.check_call(
            ["openssl", "dgst", "-sha256", "-verify", ppath, "-keyform", "DER", "-signature", spath, dpath],
            stdout=subprocess.DEVNULL,
        )
    finally:
        for p in (ppath, spath, dpath):
            os.unlink(p)


def build_rro(manifest_path: str, res_dir: str, out_apk: str):
    with tempfile.TemporaryDirectory() as td:
        raw_apk = os.path.join(td, "raw.apk")
        aligned_apk = os.path.join(td, "aligned.apk")
        subprocess.check_call([
            "aapt", "package", "-f",
            "-M", manifest_path,
            "-S", res_dir,
            "-I", FRAMEWORK_RES,
            "-F", raw_apk,
            "-0", "arsc",
        ])
        zipalign_apk(raw_apk, aligned_apk)
        sign_apk_v2(aligned_apk, out_apk)
    print(f"Built & APK-v2 signed RRO overlay: {out_apk} ({os.path.getsize(out_apk)} bytes)")


def main():
    ensure_key_and_cert()
    build_rro(
        os.path.join(KIT_DIR, "AndroidManifest.xml"),
        os.path.join(KIT_DIR, "res"),
        OUT_APK,
    )
    settings_dir = os.path.join(KIT_DIR, "settings_overlay")
    if os.path.isdir(settings_dir):
        settings_apk = os.path.join(KIT_DIR, "Neo9SettingsOverlay.apk")
        build_rro(
            os.path.join(settings_dir, "AndroidManifest.xml"),
            os.path.join(settings_dir, "res"),
            settings_apk,
        )


if __name__ == "__main__":
    main()
