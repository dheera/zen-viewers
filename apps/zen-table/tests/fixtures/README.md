# Native table regression fixtures

These small, original fixtures are checked in so the core data tests need no Python or PyArrow at execution time. Run from the repository root:

```sh
sh apps/zen-table/tests/data-smoke.sh build/zen-table
```

The runner uses the built viewer and ordinary Linux shell/core utilities. It also generates CSV/TSV input to check Unicode, quoting, multiline records, invalid input and a distant page in a 100,001-row table.

| Fixture | Contents |
| --- | --- |
| `typed-snappy.parquet` | Six original rows, 18 columns, three row groups; Snappy, dictionary encoding, data pages v1 |
| `typed-gzip.parquet` | Same values; Gzip, plain encoding, data pages v2 |
| `typed-zstd.parquet` | Same values; Zstd, dictionary encoding, data pages v2 |
| `typed-lz4.parquet` | Same values; LZ4 raw, plain encoding, data pages v1 |
| `typed-brotli.parquet` | Same values; Brotli, dictionary encoding, data pages v2 |
| `empty.parquet` | The same 18-column schema with no rows |
| `navigation.parquet` | 257 rows in five groups, for page boundaries, end clipping, search and UI checks |
| `page-budget.parquet` | About 3 KB compressed; 128 strings expand to 25.6 MB, testing the 16 MiB response limit |
| `int96-calendar.parquet` | Nine legacy INT96 timestamps: null, pre-epoch, nanosecond formatting, 1677/2262 integer-nanosecond boundaries and calendar years 1500/2500 |
| `search-retention.parquet` | One 8,192-row group with many small, non-dictionary pages: 512 MiB of string values compressed to a compact file, searched under a 512 MiB virtual-memory limit |

The typed fixtures cover signed and unsigned integers (including values above `INT64_MAX`), UTF-8, booleans, doubles, binary, decimal128/decimal256, date, time, UTC and local timestamps, nullable lists, structs, maps, nested lists of structs, empty collections and all-null columns. Maps retain ordering and duplicate keys. Page checksums are present. Expected text is defined independently from the viewer and uses TSV quoting where needed.

The retained-page check uses `ulimit -v` when available. Optional sanitizer runs can set `ZEN_TABLE_TEST_MEMORY_LIMIT_KB=0` because AddressSanitizer reserves a much larger virtual address space; the search correctness check still runs. Regenerating the compressed retention fixture temporarily allocates about 512 MiB in the development writer.

`SHA256SUMS` records the binary fixture hashes. The developer-only generator is `../generate-fixtures.py`, initially run with PyArrow 23.0.1. It creates these synthetic values, verifies their PyArrow readback, and writes the intended display text. Regeneration requires PyArrow; using the checked-in tests does not. Fixture bytes may change between writer versions even when values are identical.

All fixtures and their generator are original project data under the repository license.
