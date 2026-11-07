#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Fixed Parquet fixtures and native CSV/TSV regression checks. No Python needed.
set -eu
export LC_ALL=C.UTF-8
binary=${1:-build/zen-table}
case "$binary" in /*) ;; *) binary="$PWD/$binary" ;; esac
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fixtures="$here/fixtures"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/zen-table-data.XXXXXX")
trap 'rm -rf -- "$tmp"' 0 HUP INT TERM

inspect() { timeout 30 "$binary" --inspect "$@"; }
equal() {
    if ! cmp -s "$1" "$2"; then
        diff -u "$1" "$2" >&2 || true
        printf 'FAIL: %s\n' "$3" >&2
        exit 1
    fi
}
last_line() {
    actual=$(tail -n 1 "$tmp/out")
    if [ "$actual" != "$1" ]; then
        printf 'FAIL: expected [%s], got [%s]\n' "$1" "$actual" >&2
        exit 1
    fi
}
reject() {
    pattern=$1
    shift
    if inspect "$@" >"$tmp/out" 2>"$tmp/error"; then
        printf 'FAIL: accepted invalid or oversized input %s\n' "$1" >&2
        exit 1
    fi
    if ! grep -Eiq "$pattern" "$tmp/error"; then
        cat "$tmp/error" >&2
        printf 'FAIL: expected a useful rejection for %s\n' "$1" >&2
        exit 1
    fi
}

# Quoting, embedded newlines/tabs, Unicode, UTF-8 BOM and CRLF records.
printf '\357\273\277Name,Notes,Count\r\nCafé,"line one\nline two, ""quoted""",4\r\n"tab\there",,5\r\n,trailing,\r\nStraße,岩,6\r\n' >"$tmp/quoted.csv"
printf '\357\273\277Name\tNotes\tCount\r\nCafé\t"line one\nline two, ""quoted"""\t4\r\n"tab\there"\t\t5\r\n\ttrailing\t\r\nStraße\t岩\t6\r\n' >"$tmp/quoted.tsv"
printf '4 rows, 3 columns, delimited text\nName\tNotes\tCount\nCafé\t"line one\nline two, ""quoted"""\t4\n"tab\there"\t\t5\n\ttrailing\t\nStraße\t岩\t6\n' >"$tmp/quoted.expected"
for suffix in csv tsv; do
    inspect "$tmp/quoted.$suffix" 0 128 >"$tmp/out"
    equal "$tmp/quoted.expected" "$tmp/out" "$suffix quoting and Unicode"
    inspect "$tmp/quoted.$suffix" 0 0 STRASSE >"$tmp/out"
    last_line 'match: 4'
done
printf 'name\nfirst,extra\nlast\n' >"$tmp/ragged.csv"
printf '2 rows, 2 columns, delimited text\nname\tColumn 2\nfirst\textra\nlast\t\n' >"$tmp/ragged.expected"
inspect "$tmp/ragged.csv" 0 128 >"$tmp/out"
equal "$tmp/ragged.expected" "$tmp/out" 'ragged CSV padding'
printf 'PAR1,value\nplain,text\n' >"$tmp/signature.csv"
inspect "$tmp/signature.csv" 0 128 >"$tmp/out"
last_line "$(printf 'plain\ttext')"
awk 'BEGIN { print "index,value"; for (i=0;i<100001;i++) printf "%d,value-%d\n",i,i }' >"$tmp/large.csv"
inspect "$tmp/large.csv" 99999 2 >"$tmp/out"
printf '100001 rows, 2 columns, delimited text\nindex\tvalue\n99999\tvalue-99999\n100000\tvalue-100000\n' >"$tmp/large.expected"
equal "$tmp/large.expected" "$tmp/out" 'CSV distant-page access'
printf 'name,value\n1,"broken' >"$tmp/bad.csv"
reject 'Unclosed' "$tmp/bad.csv"
printf 'name,value\n1,\377\n' >"$tmp/replaced.csv"
inspect "$tmp/replaced.csv" 0 128 >"$tmp/out"
last_line "$(printf '1\t�')"
printf 'name,value\n1,\000\n' >"$tmp/nul.csv"
reject 'UTF-8' "$tmp/nul.csv"
mkfifo "$tmp/pipe.csv"
reject 'regular' "$tmp/pipe.csv"

# Each codec independently exercises nulls, dictionary/plain values, nested
# repetition levels, unsigned integers, exact decimals, and time/date values.
for codec in snappy gzip zstd lz4 brotli; do
    inspect "$fixtures/typed-$codec.parquet" 0 128 >"$tmp/out"
    equal "$fixtures/typed.expected" "$tmp/out" "$codec Parquet values"
    inspect "$fixtures/typed-$codec.parquet" 4 0 STRASSE >"$tmp/out"
    last_line 'match: 2'
    inspect "$fixtures/typed-$codec.parquet" 0 0 does-not-exist >"$tmp/out"
    last_line 'match: -1'
done
inspect "$fixtures/empty.parquet" 0 128 >"$tmp/out"
equal "$fixtures/empty.expected" "$tmp/out" 'empty Parquet schema'
inspect "$fixtures/int96-calendar.parquet" 0 128 >"$tmp/out"
equal "$fixtures/int96-calendar.expected" "$tmp/out" 'INT96 calendar range and nanosecond precision'
inspect "$fixtures/navigation.parquet" 63 3 >"$tmp/out"
printf '257 rows, 3 columns, Parquet\nName\tNotes\tCount\nrow-63\tnote 63\t126\nrow-64\tnote 64\t128\nrow-65\tnote 65\t130\n' >"$tmp/navigation.expected"
equal "$tmp/navigation.expected" "$tmp/out" 'Parquet row-group boundary'
inspect "$fixtures/navigation.parquet" 256 128 >"$tmp/out"
printf '257 rows, 3 columns, Parquet\nName\tNotes\tCount\nrow-256\tnote 256\t512\n' >"$tmp/navigation.expected"
equal "$tmp/navigation.expected" "$tmp/out" 'Parquet final-page clipping'
inspect "$fixtures/navigation.parquet" 0 0 row-256 >"$tmp/out"
last_line 'match: 257'
inspect "$fixtures/navigation.parquet" 255 0 row-2 >"$tmp/out"
last_line 'match: 256'
# A missing-value search must stream through 512 MiB of plain string pages
# without retaining the entire decoded row group. Sanitizers reserve a large
# virtual address space; their optional runs can set the limit to zero.
memory_limit=${ZEN_TABLE_TEST_MEMORY_LIMIT_KB:-524288}
if [ "$memory_limit" != 0 ] && (ulimit -v "$memory_limit") 2>/dev/null; then
    (ulimit -v "$memory_limit"; inspect "$fixtures/search-retention.parquet" 0 0 definitely-missing) >"$tmp/out"
else
    inspect "$fixtures/search-retention.parquet" 0 0 definitely-missing >"$tmp/out"
fi
last_line 'match: -1'
reject 'budget|limit' "$fixtures/page-budget.parquet" 0 128
printf 'PAR1bogus' >"$tmp/bad.parquet"
reject '.' "$tmp/bad.parquet"
dd if="$fixtures/typed-snappy.parquet" of="$tmp/truncated.parquet" bs=1 count=80 2>/dev/null
reject '.' "$tmp/truncated.parquet"
printf '%s\n' 'PASS native CSV/TSV quoting, Unicode, search, distant rows and invalid input; Parquet typed/nested/null values, INT96 calendar range, five codecs, dictionary/plain pages v1/v2, row groups, empty data, bounded search and response limits'
