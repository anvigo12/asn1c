#!/bin/sh

# Regression test for the asn1c exit status (see asn1c(1), EXIT STATUS).
# A FATAL diagnostic shall always give a non-zero exit status.

set -eu

top_srcdir=$(cd "${top_srcdir:-../..}" && pwd)
top_builddir=$(cd "${top_builddir:-../..}" && pwd)

ASN1C="${top_builddir}/asn1c/asn1c"
SKELETONS="${top_srcdir}/skeletons"
T="${top_srcdir}/tests/tests-asn1c-compiler"

TMPDIR_TEST=$(mktemp -d)
trap 'rm -rf "$TMPDIR_TEST"' EXIT

failures=0

# expect_status <name> <expected-status> [--fatal|--no-fatal|--fatal-with TEXT|--without TEXT] <asn1c arguments...>
# With --fatal, stderr shall contain a FATAL diagnostic.
# With --fatal-with TEXT, stderr shall contain a FATAL diagnostic with TEXT.
# With --no-fatal, stderr shall contain no FATAL diagnostic.
# With --without TEXT, stderr shall not contain TEXT.
expect_status() {
    name="$1"
    expected="$2"
    shift 2
    want_fatal=any
    fatal_text=""
    without_text=""
    case "${1:-}" in
    --fatal) want_fatal=yes; shift ;;
    --fatal-with) want_fatal=yes; fatal_text="$2"; shift 2 ;;
    --no-fatal) want_fatal=no; shift ;;
    --without) without_text="$2"; shift 2 ;;
    esac

    mkdir -p "$TMPDIR_TEST/$name"
    set +e
    (cd "$TMPDIR_TEST/$name" && "$ASN1C" -S "$SKELETONS" "$@") \
        >"$TMPDIR_TEST/$name.out" 2>"$TMPDIR_TEST/$name.err"
    status=$?
    set -e

    if [ "$status" -ne "$expected" ]; then
        echo "FAIL: $name: exit status $status, expected $expected"
        cat "$TMPDIR_TEST/$name.err" >&2
        failures=$((failures + 1))
        return 0
    fi
    if [ "$want_fatal" = yes ] \
        && ! grep "^FATAL: " "$TMPDIR_TEST/$name.err" >/dev/null; then
        echo "FAIL: $name: no FATAL diagnostic on stderr"
        failures=$((failures + 1))
        return 0
    fi
    if [ -n "$fatal_text" ] \
        && ! grep "^FATAL: .*$fatal_text" "$TMPDIR_TEST/$name.err" >/dev/null; then
        echo "FAIL: $name: no FATAL diagnostic with \"$fatal_text\""
        cat "$TMPDIR_TEST/$name.err" >&2
        failures=$((failures + 1))
        return 0
    fi
    if [ -n "$without_text" ] \
        && grep "$without_text" "$TMPDIR_TEST/$name.err" >/dev/null; then
        echo "FAIL: $name: stderr contains \"$without_text\""
        failures=$((failures + 1))
        return 0
    fi
    if [ "$want_fatal" = no ] \
        && grep "^FATAL: " "$TMPDIR_TEST/$name.err" >/dev/null; then
        echo "FAIL: $name: unexpected FATAL diagnostic on stderr"
        cat "$TMPDIR_TEST/$name.err" >&2
        failures=$((failures + 1))
        return 0
    fi
    echo "PASS: $name (exit status $status)"
}

OK="$T/03-enum-OK.asn1"
NP="$T/02-garbage-NP.asn1"
SE="$T/04-enum-SE.asn1"
SE_CLASS="$T/102-class-ref-SE.asn1"        # fixer returns failure
NOT_EXPORTED="$T/exit-status/imports-not-exported-second.asn1"
UNRETURNED="$T/exit-status/imports-not-exported-param.asn1"  # fixer reports FATAL, returns success
DUP_OID="$T/exit-status/imports-same-module-oid.asn1"
DUP_NAME="$T/exit-status/imports-same-module.asn1"
NAMEFORM="$T/exit-status/imports-nameform-oids.asn1"
EXPORTS_OK="$T/16-constraint-OK.asn1"     # own non-exported symbol in a constraint
CLASH="$T/72-same-names-OK.asn1"          # C name clash without -fcompound-names
PARAM="$T/165-param-class-governed-objectset-OK.asn1"
REFS_UNDEFINED="$T/exit-status/refs-undefined-in-generic.asn1"
REFS_UNLISTED="$T/exit-status/refs-unlisted-in-generic.asn1"
REFS_COMPONENT="$T/exit-status/refs-unknown-component.asn1"
REFS_OK="$T/187-resolved-references-OK.asn1"

expect_status ok-parse            0  --no-fatal -E "$OK"
expect_status ok-fix              0  --no-fatal -E -F "$OK"
expect_status ok-compile          0  --no-fatal -no-gen-example "$OK"
expect_status no-input-files      64
expect_status missing-file        66 -E "$TMPDIR_TEST/does-not-exist.asn1"
expect_status syntax-error        65 -E "$NP"
expect_status semantic-error      65 --fatal -E -F "$SE"
expect_status class-ref-error    65 --fatal -E -F "$SE_CLASS"
expect_status imported-not-exported 65 --fatal-with "does not mention Y" -E -F "$NOT_EXPORTED"
expect_status unreturned-fatal    65 --fatal-with "does not mention Y" -E -F -fcompound-names "$UNRETURNED"
expect_status unreturned-fatal-compile 65 --fatal-with "does not mention Y" -fcompound-names -no-gen-example "$UNRETURNED"
expect_status imports-same-module-oid 65 --fatal-with "13.16 e" -E -F -fcompound-names "$DUP_OID"
expect_status imports-same-module 65 --fatal-with "13.16 e" -E -F -fcompound-names "$DUP_NAME"
expect_status imports-nameform-oids 65 --without "13.16 e" -E -F -fcompound-names "$NAMEFORM"
expect_status unexported-own-sym  0  --no-fatal -E -F "$EXPORTS_OK"
expect_status param-fix           0  --no-fatal -E -F "$PARAM"
expect_status param-compile       0  --no-fatal -no-gen-example "$PARAM"
expect_status clash-print-fatal   70 --fatal -P "$CLASH"
# -E -F -print-json resolves every reference (the reference pass).
expect_status refs-ok             0  --no-fatal -E -F -print-json "$REFS_OK"
expect_status refs-undefined      65 --fatal-with "Cannot resolve reference \"Undefined\"" -E -F -print-json "$REFS_UNDEFINED"
expect_status refs-unlisted       65 --fatal-with "Cannot resolve reference \"Y\"" -E -F -fprefer-import-source -print-json "$REFS_UNLISTED"
expect_status refs-unknown-component 65 --fatal-with "component in WITH COMPONENTS \"c\"" -E -F -print-json "$REFS_COMPONENT"
expect_status refs-unknown-relation 65 --fatal-with "component relation \"@.nosuch\"" -E -F -print-json "$REFS_COMPONENT"
# The EXPORTS check inside a lookup of the reference pass (it crashed before).
expect_status refs-not-exported   65 --fatal-with "does not mention Y" -E -F -fcompound-names -print-json "$UNRETURNED"
# Reference pass: parameter governors (X.683 (02/2021) 8.3), components
# that an unexpanded COMPONENTS OF gives (X.680 (02/2021) 25.5, 51.8.6),
# and references to a parameterized assignment without actual parameters
# (X.683 9.2). The governor-undefined and components-of checks keep errors
# that the pass found before this change. Each FATAL text is printed once.
REFS_GOVERNORS="$T/190-param-governors-OK.asn1"
REFS_NO_ARGS="$T/exit-status/refs-param-no-args.asn1"
REFS_NO_ARGS_GENERIC="$T/exit-status/refs-param-no-args-generic.asn1"
REFS_GOVERNED_DUMMY="$T/exit-status/refs-dummy-governor-governed.asn1"
REFS_GOVERNOR_UNDEFINED="$T/exit-status/refs-governor-undefined.asn1"
REFS_COF_ADDITION="$T/exit-status/refs-components-of-addition.asn1"
REFS_COF_UNDEFINED="$T/exit-status/refs-components-of-undefined.asn1"
expect_status refs-governors      0  --no-fatal -E -F -fcompound-names -print-json "$REFS_GOVERNORS"
expect_status refs-param-no-args  65 --fatal-with "without actual parameters (X.683 9.2) (in ModuleRefsParamNoArgs.Y)" -E -F -fcompound-names -print-json "$REFS_NO_ARGS"
expect_status refs-param-no-args-generic 65 --fatal-with "without actual parameters (X.683 9.2) (in ModuleRefsParamNoArgsGeneric.V)" -E -F -fcompound-names -print-json "$REFS_NO_ARGS_GENERIC"
expect_status refs-dummy-governor-governed 65 --fatal-with "names a DummyReference that has a governor" -E -F -fcompound-names -print-json "$REFS_GOVERNED_DUMMY"
expect_status refs-governor-undefined 65 --fatal-with "parameter governor \"Nosuch\"" -E -F -fcompound-names -print-json "$REFS_GOVERNOR_UNDEFINED"
expect_status refs-components-of-addition 65 --fatal-with "component in WITH COMPONENTS \"c\"" -E -F -fcompound-names -print-json "$REFS_COF_ADDITION"
expect_status refs-components-of-undefined 65 --fatal-with "\"Nosuch\"" -E -F -fcompound-names -print-json "$REFS_COF_UNDEFINED"
expect_status refs-fatal-once     65 --fatal-with "component in WITH COMPONENTS \"c\"" -E -F -fcompound-names -print-json "$REFS_COMPONENT"
if [ -f "$TMPDIR_TEST/refs-fatal-once.err" ]; then
    n=$(grep -c "^FATAL: .*component in WITH COMPONENTS \"c\"" "$TMPDIR_TEST/refs-fatal-once.err" || true)
    if [ "$n" -eq 1 ]; then
        echo "PASS: refs-fatal-once-count (1 FATAL line)"
    else
        echo "FAIL: refs-fatal-once-count: $n FATAL lines, expected 1"
        failures=$((failures + 1))
    fi
fi

if [ "$failures" -ne 0 ]; then
    echo "$failures exit status check(s) failed"
    exit 1
fi
