#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Developer-only regeneration; running the checked-in tests needs no Python.

Requires PyArrow with Snappy, Gzip, Zstd, LZ4 and Brotli support. All data here is
original, synthetic test data. Expected text describes Zen Table's display
contract and is generated from these values, never from the viewer itself.
"""
import csv
from datetime import date, datetime, time, timezone
from decimal import Decimal
import hashlib
import io
import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq

root = Path(__file__).resolve().parent / "fixtures"
root.mkdir(exist_ok=True)
schema = pa.schema([
    ("id", pa.int64()), ("name", pa.string()), ("enabled", pa.bool_()),
    ("count", pa.int32()), ("ratio", pa.float64()), ("binary", pa.binary()),
    ("amount", pa.decimal128(12, 2)), ("large_amount", pa.decimal256(45, 7)),
    ("day", pa.date32()), ("clock", pa.time64("us")),
    ("instant", pa.timestamp("us", "UTC")), ("local_time", pa.timestamp("ms")),
    ("numbers", pa.list_(pa.int32())),
    ("profile", pa.struct([("name", pa.string()), ("n", pa.int32())])),
    ("pairs", pa.map_(pa.string(), pa.int64())),
    ("nested", pa.list_(pa.struct([("tag", pa.string()), ("samples", pa.list_(pa.float64()))]))),
    ("all_null", pa.int16()), ("u64", pa.uint64()),
])
rows = [
    dict(id=0, name="Café", enabled=True, count=-7, ratio=1.5, binary=b"\x00\xff",
         amount=Decimal("12.34"), large_amount=Decimal("123456789012345678901234567890.1234567"),
         day=date(2026, 4, 12), clock=time(3, 4, 5, 6000),
         instant=datetime(2026, 4, 12, tzinfo=timezone.utc), local_time=datetime(2026, 4, 12, 3, 4, 5, 6000),
         numbers=[1, None, 3], profile={"name": "x", "n": 2}, pairs=[("k", 1), ("q", None)],
         nested=[{"tag": "deep", "samples": [1.5, None, -2.25]}], all_null=None, u64=18446744073709551615),
    dict(id=1, name="Straße", enabled=False, count=2147483647, ratio=-2.25, binary=b"",
         amount=Decimal("-0.01"), large_amount=Decimal("-1.0000001"),
         day=date(1969, 12, 31), clock=time(23, 59, 59, 999999),
         instant=datetime(1969, 12, 31, 23, 59, 59, 500000, timezone.utc), local_time=None,
         numbers=[], profile={"name": None, "n": None}, pairs=[], nested=[], all_null=None, u64=9223372036854775808),
    dict(id=2, name=None, enabled=None, count=None, ratio=None, binary=None, amount=None, large_amount=None,
         day=None, clock=None, instant=None, local_time=None, numbers=None, profile=None,
         pairs=None, nested=None, all_null=None, u64=None),
    dict(id=3, name='岩\t"quoted"\nnext', enabled=True, count=-2147483648, ratio=0.125, binary=b"abc",
         amount=Decimal("0.00"), large_amount=Decimal("0.0000000"),
         day=date(1970, 1, 1), clock=time(0, 0), instant=datetime(1970, 1, 1, tzinfo=timezone.utc),
         local_time=datetime(2000, 1, 1), numbers=[None], profile={"name": "岩", "n": -1},
         pairs=[("岩", 42)], nested=[None, {"tag": None, "samples": None}, {"tag": "", "samples": []}],
         all_null=None, u64=0),
    dict(id=4, name="", enabled=False, count=0, ratio=1024.5, binary=b"\x10",
         amount=Decimal("9999999999.99"), large_amount=Decimal("-123456789012345678901234567890.1234567"),
         day=date(2000, 2, 29), clock=time(12, 30, 0, 1), instant=datetime(2000, 2, 29, 12, 30, 0, 1, timezone.utc),
         local_time=datetime(2000, 2, 29, 12, 30, 0, 1_000), numbers=[4, 5], profile={"name": "", "n": 0},
         pairs=[("same", 1), ("same", 2)], nested=[{"tag": "pair", "samples": []}, {"tag": "null", "samples": [None]}],
         all_null=None, u64=123),
    dict(id=5, name="last-row", enabled=True, count=42, ratio=3.25, binary=b"\x7f\x80",
         amount=Decimal("-99.90"), large_amount=Decimal("42.1234567"),
         day=date(2030, 12, 31), clock=time(6, 7, 8), instant=datetime(2030, 12, 31, 6, 7, 8, tzinfo=timezone.utc),
         local_time=datetime(2030, 12, 31, 6, 7, 8), numbers=[9], profile={"name": "last", "n": 5},
         pairs=[("final", 5)], nested=[{"tag": "end", "samples": [0.5]}], all_null=None, u64=4294967296),
]
table = pa.Table.from_pylist(rows, schema=schema)
settings = [
    ("snappy", "1.0", True), ("gzip", "2.0", False), ("zstd", "2.0", True),
    ("lz4", "1.0", False), ("brotli", "2.0", True),
]
for codec, page_version, dictionary in settings:
    assert pa.Codec.is_available(codec), codec
    pq.write_table(table, root / f"typed-{codec}.parquet", compression=codec,
                   data_page_version=page_version, use_dictionary=dictionary,
                   row_group_size=2, data_page_size=256, write_batch_size=2,
                   version="2.6", write_page_checksum=True)
    assert pq.read_table(root / f"typed-{codec}.parquet").equals(table)
pq.write_table(table.slice(0, 0), root / "empty.parquet", compression="snappy")
navigation = pa.table({"Name": [f"row-{i}" for i in range(257)],
                       "Notes": [f"note {i}" for i in range(257)], "Count": [i * 2 for i in range(257)]})
pq.write_table(navigation, root / "navigation.parquet", row_group_size=64, compression="zstd")
pq.write_table(pa.table({"payload": ["x" * 200_000] * 128}), root / "page-budget.parquet",
               compression="zstd", use_dictionary=False, write_statistics=False,
               row_group_size=128, data_page_size=1 << 20, write_batch_size=8)
retained_pages = pa.table({"id": range(8192), "text": ["x" * 65536] * 8192})
pq.write_table(retained_pages, root / "search-retention.parquet", compression="zstd",
               use_dictionary=False, write_statistics=False, row_group_size=8192,
               data_page_size=65536, write_batch_size=4)
del retained_pages
int96_rows = [
    ("lower-day", datetime(1677, 9, 21)),
    ("upper-day", datetime(2262, 4, 11, 23, 59, 59)),
    ("lower-instant", datetime(1677, 9, 21, 0, 12, 43, 145225)),
    ("upper-instant", datetime(2262, 4, 11, 23, 47, 16, 854775)),
    ("early-calendar", datetime(1500, 1, 1)),
    ("late-calendar", datetime(2500, 1, 1)),
    ("modern", datetime(2026, 4, 12, 3, 4, 5, 123456)),
    ("pre-epoch", datetime(1969, 12, 31, 23, 59, 59, 500000)),
    ("null", None),
]
int96_table = pa.table({"label": [item[0] for item in int96_rows],
                        "instant": pa.array([item[1] for item in int96_rows], type=pa.timestamp("us"))})
pq.write_table(int96_table, root / "int96-calendar.parquet", compression="zstd", row_group_size=3,
               use_dictionary=False, use_deprecated_int96_timestamps=True)
# Default nanosecond coercion cannot represent years 1500/2500; microsecond
# readback verifies the original calendar values without that narrowing.
assert pq.read_table(root / "int96-calendar.parquet", coerce_int96_timestamp_unit="us").equals(int96_table)
int96_expected = io.StringIO(newline="")
int96_expected.write(f"{len(int96_rows)} rows, 2 columns, Parquet\n")
int96_writer = csv.writer(int96_expected, delimiter="\t", lineterminator="\n")
int96_writer.writerow(["label", "instant"])
for label, value in int96_rows:
    rendered = "" if value is None else value.isoformat(sep=" ", timespec="seconds")
    if value is not None and value.microsecond:
        rendered += f".{value.microsecond * 1000:09d}"
    int96_writer.writerow([label, rendered])
(root / "int96-calendar.expected").write_text(int96_expected.getvalue(), encoding="utf-8")


def render(value, field):
    if value is None:
        return ""
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, bytes):
        return "0x" + value.hex()
    if isinstance(value, Decimal):
        return format(value, "f")
    if isinstance(value, (dict, list, tuple)):
        return json.dumps(value, ensure_ascii=False, default=str, separators=(", ", ": "))
    if isinstance(value, datetime):
        precision = "milliseconds" if pa.types.is_timestamp(field.type) and field.type.unit == "ms" else "microseconds"
        return value.isoformat(sep=" ", timespec=precision if value.microsecond else "seconds")
    return str(value)


def expected(data):
    output = io.StringIO(newline="")
    output.write(f"{len(data)} rows, {len(schema)} columns, Parquet\n")
    writer = csv.writer(output, delimiter="\t", lineterminator="\n")
    writer.writerow(schema.names)
    writer.writerows([render(row[field.name], field) for field in schema] for row in data)
    return output.getvalue()


(root / "typed.expected").write_text(expected(rows), encoding="utf-8")
(root / "empty.expected").write_text(expected([]), encoding="utf-8")
manifest = []
for path in sorted(root.glob("*.parquet")):
    data = path.read_bytes()
    manifest.append(f"{hashlib.sha256(data).hexdigest()}  {path.name}")
    print(f"{path.name}: {len(data)} bytes")
(root / "SHA256SUMS").write_text("\n".join(manifest) + "\n")
print(f"Generated with PyArrow {pa.__version__}; tests consume fixed files without importing it.")
