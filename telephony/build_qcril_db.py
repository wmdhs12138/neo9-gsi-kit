#!/usr/bin/env python3
"""
Extract /vendor/etc/qcril_database/qcrilNr.db from a vivo iQOO Neo9 (PD2338)
(or from a provided input path) and generate telephony/neo9_qcrilNr.db with
`persist.vendor.radio.poweron_opt` set to '0'.

Why:
  qcrilNrd reads `persist.vendor.radio.poweron_opt` via `qcril_config_get()`
  (libril-db.so -> SQLite `qcril_properties_table`), NOT Android system props.
  When poweron_opt='1' without OriginOS's QtiTelephony sending OEM_HOOK UI_READY,
  libqcrilNrSmsModule buffers all incoming MT SMS indefinitely without ACK.
"""
import argparse
import os
import shutil
import sqlite3
import subprocess
import tempfile

TEL_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OUT = os.path.join(TEL_DIR, "neo9_qcrilNr.db")
DEVICE_DB = "/vendor/etc/qcril_database/qcrilNr.db"


def main():
    ap = argparse.ArgumentParser(description="Build patched PD2338 qcrilNr.db (poweron_opt=0)")
    ap.add_argument("--input", "-i", help="Path to stock qcrilNr.db (default: pull from /vendor via su)")
    ap.add_argument("--output", "-o", default=DEFAULT_OUT, help=f"Output SQLite DB path (default: {DEFAULT_OUT})")
    args = ap.parse_args()

    if args.input:
        shutil.copyfile(args.input, args.output)
    else:
        with tempfile.NamedTemporaryFile(delete=False) as tf:
            tmp = tf.name
        try:
            subprocess.check_call(
                ["su", "-c", f"cp {DEVICE_DB} /data/local/tmp/_neo9_qcril_raw.db && chmod 644 /data/local/tmp/_neo9_qcril_raw.db"]
            )
            shutil.copyfile("/data/local/tmp/_neo9_qcril_raw.db", args.output)
        finally:
            subprocess.call(["su", "-c", "rm -f /data/local/tmp/_neo9_qcril_raw.db"])
            if os.path.exists(tmp):
                os.unlink(tmp)

    conn = sqlite3.connect(args.output)
    cur = conn.cursor()
    before = cur.execute(
        "SELECT property, def_val, value FROM qcril_properties_table WHERE property='persist.vendor.radio.poweron_opt'"
    ).fetchall()
    cur.execute(
        "UPDATE qcril_properties_table SET def_val='0', value='0' WHERE property='persist.vendor.radio.poweron_opt'"
    )
    conn.commit()
    after = cur.execute(
        "SELECT property, def_val, value FROM qcril_properties_table WHERE property='persist.vendor.radio.poweron_opt'"
    ).fetchall()
    chk = cur.execute("PRAGMA integrity_check").fetchall()
    conn.close()

    assert after == [("persist.vendor.radio.poweron_opt", "0", "0")], f"Unexpected row after patch: {after}"
    assert chk == [("ok",)], f"SQLite integrity check failed: {chk}"
    print(f"Patched {args.output}: {before} -> {after} (integrity=ok)")


if __name__ == "__main__":
    main()
