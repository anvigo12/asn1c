#!/bin/sh

# Checks of "asn1c -E -print-json" that reference outputs cannot express:
# the nesting depth limit, and string values that are not UTF-8.

set -eu

top_srcdir=$(cd "${top_srcdir:-../..}" && pwd)
top_builddir=$(cd "${top_builddir:-../..}" && pwd)

ASN1C="${top_builddir}/asn1c/asn1c"
SKELETONS="${top_srcdir}/skeletons"

TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT
failures=0

fail() {
    echo "FAIL: $*"
    failures=$((failures + 1))
}

# 1. Depth limit: 300 nested SEQUENCE types exceed the printer limit.
deep="$TMPDIR_TEST/deep.asn1"
{
    echo "ModuleDeep DEFINITIONS ::= BEGIN T ::="
    i=0; while [ $i -lt 300 ]; do printf 'SEQUENCE { a '; i=$((i + 1)); done
    printf 'INTEGER'
    i=0; while [ $i -lt 300 ]; do printf ' }'; i=$((i + 1)); done
    echo; echo "END"
} > "$deep"
set +e
"$ASN1C" -S "$SKELETONS" -E -print-json "$deep" > "$TMPDIR_TEST/deep.json" 2> "$TMPDIR_TEST/deep.err"
status=$?
set -e
if [ "$status" -eq 0 ]; then
    fail "depth-limit: exit status 0"
elif ! grep "^FATAL: -print-json: .*depth limit" "$TMPDIR_TEST/deep.err" > /dev/null; then
    fail "depth-limit: no FATAL diagnostic that names the limit"
    cat "$TMPDIR_TEST/deep.err" >&2
else
    echo "PASS: depth-limit (exit status $status)"
fi

# 2. A string value with a Latin-1 byte (0xE9): mapped text, exact bytes, and a warning.
latin="$TMPDIR_TEST/latin1.asn1"
printf 'ModuleLatin DEFINITIONS ::= BEGIN s VisibleString ::= "caf\351" END\n' > "$latin"
set +e
"$ASN1C" -S "$SKELETONS" -E -print-json "$latin" > "$TMPDIR_TEST/latin1.json" 2> "$TMPDIR_TEST/latin1.err"
status=$?
set -e
if [ "$status" -ne 0 ]; then
    fail "not-utf8: exit status $status"
elif ! grep '"stringBytes": "Y2Fm6Q=="' "$TMPDIR_TEST/latin1.json" > /dev/null; then
    fail "not-utf8: stringBytes missing or wrong"
    cat "$TMPDIR_TEST/latin1.json" >&2
elif ! grep '"string": "caf\\u00e9"' "$TMPDIR_TEST/latin1.json" > /dev/null; then
    fail "not-utf8: mapped string missing or wrong"
elif ! grep "^WARNING: -print-json: string value .* not valid UTF-8" "$TMPDIR_TEST/latin1.err" > /dev/null; then
    fail "not-utf8: no WARNING diagnostic"
else
    echo "PASS: not-utf8"
fi

# 3. Valid UTF-8 (U+00E9 as 0xC3 0xA9) passes through without stringBytes.
utf8="$TMPDIR_TEST/utf8.asn1"
printf 'ModuleUtf8 DEFINITIONS ::= BEGIN s UTF8String ::= "caf\303\251" END\n' > "$utf8"
"$ASN1C" -S "$SKELETONS" -E -print-json "$utf8" > "$TMPDIR_TEST/utf8.json" 2> "$TMPDIR_TEST/utf8.err"
if grep '"stringBytes"' "$TMPDIR_TEST/utf8.json" > /dev/null || [ -s "$TMPDIR_TEST/utf8.err" ]; then
    fail "utf8: unexpected stringBytes or diagnostic"
else
    echo "PASS: utf8"
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures print-json check(s) failed"
    exit 1
fi
