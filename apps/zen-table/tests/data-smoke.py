#!/usr/bin/env python3
"""Native parser and real Parquet integration regression tests."""
import csv
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
csv.field_size_limit(64 * 1024 * 1024)

binary = Path(sys.argv[1] if len(sys.argv) > 1 else "build/zen-table").resolve()

def inspect(path, start=0, count=128, needle=None, ok=True):
    args = [str(binary), "--inspect", str(path), str(start), str(count)]
    if needle is not None:
        args.append(needle)
    result = subprocess.run(args, text=True, capture_output=True, timeout=30)
    if not ok:
        assert result.returncode, result.stdout
        return result.stderr
    assert result.returncode == 0, result.stderr
    summary, data = result.stdout.split("\n", 1)
    return summary, list(csv.reader(io.StringIO(data), delimiter="\t"))

with tempfile.TemporaryDirectory(prefix="zen-table-test-") as tmp:
    root = Path(tmp)
    rows = [["Name", "Notes", "Count"], ["Café", 'line one\nline two, "quoted"', "4"],
            ["tab\there", "", "5"], ["", "trailing", ""], ["Straße", "岩", "6"]]
    for suffix, delimiter in (("csv", ","), ("tsv", "\t")):
        path = root / ("quoted." + suffix)
        with path.open("w", encoding="utf-8-sig", newline="") as f:
            csv.writer(f, delimiter=delimiter, lineterminator="\r\n").writerows(rows)
        summary, actual = inspect(path)
        assert summary == "4 rows, 3 columns, delimited text"
        assert actual == rows, actual
        assert inspect(path, needle="STRASSE")[1][-1] == ["match: 4"]
    path = root / "signature.csv"
    path.write_text("PAR1,value\nplain,text\n")
    assert inspect(path)[1] == [["PAR1", "value"], ["plain", "text"]]
    path = root / "ragged.csv"
    path.write_text("name\nfirst,extra\nlast\n")
    assert inspect(path)[1] == [["name", "Column 2"], ["first", "extra"], ["last", ""]]
    path = root / "large.csv"
    with path.open("w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(["index", "value"])
        writer.writerow([0, "x" * 200_000])
        writer.writerows((i, f"value-{i}") for i in range(1, 100_001))
    summary, actual = inspect(path, 99_999, 2)
    assert summary.startswith("100001 rows")
    assert actual[-1] == ["100000", "value-100000"]
    assert len(inspect(path, 0, 1)[1][1][1]) == 200_000
    path = root / "invalid.csv"
    path.write_bytes(b'name,value\n1,"broken')
    assert "Unclosed" in inspect(path, ok=False)
    path.write_bytes(b'name,value\n1,\xff\n')
    assert inspect(path)[1][-1] == ["1", "�"]
    path.write_bytes(b'name,value\n1,\x00\n')
    assert "UTF-8" in inspect(path, ok=False)
    fifo = root / "pipe.csv"
    os.mkfifo(fifo)
    assert "regular" in inspect(fifo, ok=False)
    bad = root / "bad.parquet"
    bad.write_bytes(b'PAR1bogus')
    assert inspect(bad, ok=False)
    try:
        import pyarrow as pa
        import pyarrow.parquet as pq
    except ImportError:
        print("CSV/TSV tests passed; Parquet test skipped (PyArrow unavailable in test Python).")
        sys.exit(0)
    from decimal import Decimal
    from datetime import datetime, timezone
    path = root / "typed.parquet"
    table = pa.table({"index": range(1000), "name": [f"row-{i}" for i in range(1000)],
                      "nested": [[1, None, 3]] * 1000,
                      "amount": [Decimal("12.34")] * 1000,
                      "binary": [b'\x00\xff'] * 1000, "empty": [None] * 1000,
                      "time": [datetime(2026, 4, 12, tzinfo=timezone.utc)] * 1000})
    pq.write_table(table, path, row_group_size=73, compression="zstd")
    summary, actual = inspect(path, 70, 128, "row-997")
    assert summary == "1000 rows, 7 columns, Parquet"
    assert actual[1][:6] == ["70", "row-70", "[1, null, 3]", "12.34", "0x00ff", ""]
    assert actual[-1] == ["match: 998"], actual[-1]
    pq.write_table(table.slice(0, 0), path)
    assert inspect(path)[0] == "0 rows, 7 columns, Parquet"
    # Force the bounded iterator route with a large uncompressed row group.
    path = root / "big-group.parquet"
    pq.write_table(pa.table({"index": range(20_000), "text": ["x" * 1000 + str(i) for i in range(20_000)]}),
                   path, row_group_size=20_000, compression="snappy", use_dictionary=False)
    assert inspect(path, 19_998, 2)[1][-1][0] == "19999"
    # High-expansion cells must fail cleanly instead of creating an unbounded page.
    path = root / "page-budget.parquet"
    pq.write_table(pa.table({"payload": ["x" * 200_000] * 128}), path, use_dictionary=False)
    assert "budget" in inspect(path, count=128, ok=False)
print("CSV/TSV quoting, multiline, BOM, ragged rows, big rows, random access, search, FIFO, malformed input, and real Parquet tests passed.")
