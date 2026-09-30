#!/usr/bin/env python3
"""Migrate ShaderBytecodeRecord.dat from v1 to v2 without changing its blob."""

import argparse
import os
from pathlib import Path
import shutil
import struct
import sys
import tempfile


MAGIC = int.from_bytes(b"SBRC", "little")
VERSION_1 = 1
VERSION_2 = 2
MAX_RECORDS = 16_384
BLOB_ALIGNMENT = 4096
HEADER = struct.Struct("<IIIIQII")
RECORD = struct.Struct("<QQIIIHBB")


def migrate(dat_path: Path, make_backup: bool) -> bool:
    raw = dat_path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError("metadata is shorter than the 32-byte header")

    magic, version, count, flags, blob_size, next_epoch, reserved = HEADER.unpack_from(raw)
    if magic != MAGIC:
        raise ValueError("metadata magic is not SBRC")
    if version == VERSION_2:
        print(f"Already version 2: {dat_path}")
        return False
    if version != VERSION_1:
        raise ValueError(f"unsupported metadata version {version}; expected v1")
    if reserved != 0:
        raise ValueError("v1 reserved header field is nonzero")
    if count > MAX_RECORDS:
        raise ValueError(f"record count {count} exceeds the ledger limit {MAX_RECORDS}")

    expected_size = HEADER.size + count * RECORD.size
    if len(raw) != expected_size:
        raise ValueError(f"metadata size is {len(raw)} bytes; expected {expected_size}")
    if blob_size % BLOB_ALIGNMENT:
        raise ValueError("recorded blob size is not 4096-byte aligned")

    previous_key = None
    for index in range(count):
        offset = HEADER.size + index * RECORD.size
        shader_hash, blob_offset, bytecode_size, last_seen, access_count, entry_flags, stage, pad = RECORD.unpack_from(raw, offset)
        del last_seen, access_count

        if stage >= 6:
            raise ValueError(f"record {index} has invalid shader stage {stage}")
        if entry_flags or pad:
            raise ValueError(f"record {index} has nonzero v1 reserved fields")
        key = (stage, shader_hash)
        if previous_key is not None and key <= previous_key:
            raise ValueError(f"record {index} breaks the sorted, unique (stage, hash) order")
        previous_key = key

        if bytecode_size:
            if blob_offset % BLOB_ALIGNMENT:
                raise ValueError(f"record {index} has an unaligned blob offset")
            if blob_offset > blob_size or bytecode_size > blob_size - blob_offset:
                raise ValueError(f"record {index} points outside the recorded blob")

    blob_path = dat_path.with_suffix(".blob")
    if blob_size:
        if not blob_path.is_file():
            raise ValueError(f"recorded bytecode blob is missing: {blob_path}")
        if blob_path.stat().st_size < blob_size:
            raise ValueError("bytecode blob is shorter than the metadata's recorded size")

    if make_backup:
        backup_path = dat_path.with_suffix(dat_path.suffix + ".v1.bak")
        if backup_path.exists():
            raise FileExistsError(f"backup already exists; refusing to overwrite: {backup_path}")
        shutil.copy2(dat_path, backup_path)

    # v2 uses the old reserved header word for arena size. Four zero bytes
    # reserve offset zero to mean "no declaration data". Record strides and
    # all bytecode blob offsets remain unchanged.
    migrated_header = HEADER.pack(MAGIC, VERSION_2, count, flags, blob_size, next_epoch, 4)
    migrated = migrated_header + raw[HEADER.size:] + b"\0\0\0\0"

    temp_path = None
    try:
        with tempfile.NamedTemporaryFile(
                mode="wb", dir=dat_path.parent, prefix=dat_path.name + ".",
                suffix=".tmp", delete=False) as temp:
            temp_path = Path(temp.name)
            temp.write(migrated)
            temp.flush()
            os.fsync(temp.fileno())
        os.replace(temp_path, dat_path)
    except Exception:
        if temp_path is not None:
            temp_path.unlink(missing_ok=True)
        raise

    print(f"Migrated {count} records: {dat_path}")
    if make_backup:
        print(f"Original metadata backup: {backup_path}")
    print(f"Bytecode blob unchanged: {blob_path}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert ShaderBytecodeRecord.dat v1 metadata to v2; the .blob is not modified.")
    parser.add_argument("cache", type=Path,
                        help="shader cache directory or its ShaderBytecodeRecord.dat file")
    parser.add_argument("--no-backup", action="store_true",
                        help="do not create the default .dat.v1.bak backup")
    args = parser.parse_args()

    dat_path = args.cache
    if dat_path.is_dir():
        dat_path = dat_path / "ShaderBytecodeRecord.dat"

    try:
        migrate(dat_path, not args.no_backup)
    except (OSError, ValueError) as exc:
        print(f"Migration failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
