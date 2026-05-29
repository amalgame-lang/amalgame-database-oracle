#!/bin/bash
# ─────────────────────────────────────────────────────
#  amalgame-database-oracle — Test Runner
#  Usage: ./tests/run_tests.sh [/path/to/amc]
#
#  Triple-gated:
#    1. Oracle Instant Client SDK (oci.h) is installed
#       (header probe via gcc -E with ORACLE_HOME include path)
#    2. libclntsh is on the linker path
#    3. An Oracle server is reachable via the env vars:
#         ORACLE_HOST     (default 127.0.0.1)
#         ORACLE_PORT     (default 1521)
#         ORACLE_USER     (e.g. system)
#         ORACLE_PASSWORD (e.g. oracle)
#         ORACLE_CONNSTR  (e.g. //127.0.0.1:1521/XEPDB1)
#
#  Start a server locally (Oracle XE 21c, ~3 GB image):
#    docker run --rm -d --name oratest -p 1521:1521 \
#      -e ORACLE_PASSWORD=oracle gvenzl/oracle-xe:21-slim
#  Then export:
#    ORACLE_USER=system ORACLE_PASSWORD=oracle
#    ORACLE_CONNSTR=//127.0.0.1:1521/XEPDB1
#
#  Tell the runner where the Instant Client SDK lives:
#    ORACLE_HOME=/opt/instantclient_21_13
# ─────────────────────────────────────────────────────

set -u

if [ $# -ge 1 ]; then
    AMC="$1"
elif [ -n "${AMC:-}" ]; then
    :
elif command -v amc >/dev/null 2>&1; then
    AMC="$(command -v amc)"
else
    echo "ERROR: amc not found." >&2
    exit 2
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PKG_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PKG_RUNTIME="$PKG_ROOT/runtime"

AMC_DIR="$(cd "$(dirname "$AMC")" && pwd)"
if [ -d "$AMC_DIR/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/runtime"
elif [ -d "$AMC_DIR/../share/amalgame/runtime" ]; then
    AMC_RUNTIME="$AMC_DIR/../share/amalgame/runtime"
elif [ -n "${AMC_RUNTIME:-}" ]; then
    :
else
    echo "ERROR: amc runtime/ not found. Set AMC_RUNTIME=..." >&2
    exit 2
fi

BUILD_DIR="$(mktemp -d -t amora-XXXXXX)"
trap 'rm -rf "$BUILD_DIR"' EXIT
PROJ_DIR="$BUILD_DIR/proj"
mkdir -p "$PROJ_DIR"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'
PASS=0; FAIL=0; SKIP=0

echo ""
echo "════════════════════════════════════════════"
echo "  amalgame-database-oracle — Tests"
echo "════════════════════════════════════════════"
echo "  amc:     $AMC ($("$AMC" --version 2>&1 | head -1))"
echo "  runtime: $AMC_RUNTIME"
echo ""

# ── Gate 1 : Instant Client SDK header present? ───
echo "── Probing OCI (oci.h) ───────────────────"
OCI_OK=0
OCI_INC=""
OCI_LIB=""

# Common Instant Client install locations.
SEARCH_DIRS=()
if [ -n "${ORACLE_HOME:-}" ];      then SEARCH_DIRS+=("$ORACLE_HOME"); fi
if [ -n "${INSTANTCLIENT:-}" ];    then SEARCH_DIRS+=("$INSTANTCLIENT"); fi
SEARCH_DIRS+=(
  /opt/instantclient_*
  /usr/lib/oracle/*/client64
  /usr/local/instantclient_*
  $HOME/instantclient_*
)

for d in "${SEARCH_DIRS[@]}"; do
    [ -d "$d" ] || continue
    if [ -f "$d/sdk/include/oci.h" ]; then
        OCI_INC="$d/sdk/include"
        OCI_LIB="$d"
        echo "  oci.h:           $OCI_INC/oci.h"
        echo "  libclntsh:       $OCI_LIB"
        OCI_OK=1
        break
    fi
    if [ -f "$d/include/oci.h" ]; then
        OCI_INC="$d/include"
        OCI_LIB="${d}/lib"
        [ -d "$OCI_LIB" ] || OCI_LIB="$d"
        echo "  oci.h:           $OCI_INC/oci.h"
        echo "  libclntsh:       $OCI_LIB"
        OCI_OK=1
        break
    fi
done

if [ "$OCI_OK" = "0" ]; then
    # Fall back to system path (some distros expose <oci.h> directly).
    echo "#include <oci.h>" > "$BUILD_DIR/_inc.c"
    if gcc -E "$BUILD_DIR/_inc.c" >/dev/null 2>&1; then
        OCI_OK=1
        echo "  oci.h:           found in system include path"
    else
        echo "  oci.h:           NOT FOUND (set ORACLE_HOME=/opt/instantclient_…)"
    fi
fi
echo ""

# ── Gate 2 : Oracle server reachable? ───────────
ORACLE_HOST="${ORACLE_HOST:-127.0.0.1}"
ORACLE_PORT="${ORACLE_PORT:-1521}"
: "${ORACLE_USER:=}"
: "${ORACLE_PASSWORD:=}"
: "${ORACLE_CONNSTR:=}"
if [ -z "$ORACLE_CONNSTR" ]; then
    ORACLE_CONNSTR="//${ORACLE_HOST}:${ORACLE_PORT}/XEPDB1"
fi
export ORACLE_USER ORACLE_PASSWORD ORACLE_CONNSTR

DB_OK=0
if (echo > /dev/tcp/$ORACLE_HOST/$ORACLE_PORT) 2>/dev/null; then
    DB_OK=1
    echo "  oracle:          reachable at $ORACLE_HOST:$ORACLE_PORT"
else
    echo "  oracle:          NOT reachable at $ORACLE_HOST:$ORACLE_PORT"
fi
echo ""

# ── Stage fake cache for the test fixture ────────────
FAKE_CACHE="$BUILD_DIR/cache"
PKG_GIT="github.com/amalgame-lang/amalgame-database-oracle"
PKG_TAG="${PKG_TAG:-v0.1.0}"
FAKE_SHA="deadbeefcafebabe0000000000000000000000ab"
SHORT_SHA="${FAKE_SHA:0:8}"
PKG_CACHE_DIR="$FAKE_CACHE/$PKG_GIT/${PKG_TAG}_${SHORT_SHA}"

mkdir -p "$(dirname "$PKG_CACHE_DIR")"
rm -rf "$PKG_CACHE_DIR"
ln -s "$PKG_ROOT" "$PKG_CACHE_DIR"

cat > "$PROJ_DIR/amalgame.lock" <<EOF
[[package]]
name = "amalgame-database-oracle"
git  = "$PKG_GIT"
tag  = "$PKG_TAG"
rev  = "$FAKE_SHA"
EOF

export AMALGAME_PACKAGES_DIR="$FAKE_CACHE"

run_test() {
    local name="$1"
    local expected="$2"
    printf "  %-38s" "$name"
    if [ "$OCI_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (Instant Client SDK not installed)"
        SKIP=$((SKIP + 1)); return
    fi
    if [ "$DB_OK" = "0" ]; then
        echo -e "${YELLOW}SKIP${NC} (no oracle at $ORACLE_HOST:$ORACLE_PORT)"
        SKIP=$((SKIP + 1)); return
    fi
    if [ -z "$ORACLE_USER" ] || [ -z "$ORACLE_PASSWORD" ]; then
        echo -e "${YELLOW}SKIP${NC} (ORACLE_USER/PASSWORD not set)"
        SKIP=$((SKIP + 1)); return
    fi
    cp "$SCRIPT_DIR/stdlib_oracle.am" "$PROJ_DIR/test.am"
    local out_base="$PROJ_DIR/test"
    local out
    out=$(cd "$PROJ_DIR" && "$AMC" -o test test.am 2>&1)
    if [ $? -ne 0 ]; then
        echo -e "${RED}FAIL${NC} (amc)"; echo "$out" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    if [ ! -f "$out_base.c" ]; then
        echo -e "${RED}FAIL${NC} (no .c)"; FAIL=$((FAIL + 1)); return
    fi
    local extra_inc=""
    local extra_lib=""
    [ -n "$OCI_INC" ] && extra_inc="-I$OCI_INC"
    [ -n "$OCI_LIB" ] && extra_lib="-L$OCI_LIB -Wl,-rpath,$OCI_LIB"
    gcc -O2 -w \
        -I"$AMC_RUNTIME" -I"$PKG_RUNTIME" $extra_inc \
        "$out_base.c" \
        $extra_lib \
        -lgc -lm -ldl -lpthread -lclntsh \
        -o "$out_base" 2>"$BUILD_DIR/link.log"
    if [ ! -x "$out_base" ]; then
        echo -e "${RED}FAIL${NC} (gcc link)"
        cat "$BUILD_DIR/link.log" | head -5 | sed 's/^/    /'
        FAIL=$((FAIL + 1)); return
    fi
    local run_output
    run_output=$("$out_base" 2>&1)
    if echo "$run_output" | grep -qF "$expected"; then
        echo -e "${GREEN}PASS${NC}"; PASS=$((PASS + 1))
    else
        echo -e "${RED}FAIL${NC}"
        echo "    expected: $expected"
        echo "    got:      $(echo "$run_output" | head -3 | tr '\n' '|')"
        FAIL=$((FAIL + 1))
    fi
}

echo "── Oracle Database ─────────────────────────"
run_test "open"                    "[PASS] open"
run_test "ServerVersion"           "[PASS] ServerVersion"
run_test "CREATE TABLE"            "[PASS] CREATE TABLE"
run_test "INSERT Changes"          "[PASS] INSERT Changes==1"
run_test "SELECT 3 rows"           "[PASS] SELECT 3 rows"
run_test "SELECT cell content"     "[PASS] SELECT cell content"
run_test "IDENTITY id starts at 1" "[PASS] IDENTITY id starts at 1"
run_test "UPDATE Changes==2"       "[PASS] UPDATE Changes==2"
run_test "bad SQL reports error"   "[PASS] bad SQL reports error"

# ── v0.3: parameter binding + transactions ──
run_test "execbind insert"           "[PASS] execbind insert"
run_test "execbind injection foiled" "[PASS] execbind injection foiled"
run_test "querybindall fetch"        "[PASS] querybindall fetch"
run_test "execbind arity check"      "[PASS] execbind arity check"
run_test "begin"                     "[PASS] begin"
run_test "rollback"                  "[PASS] rollback"
run_test "rollback drops insert"     "[PASS] rollback drops insert"
run_test "commit"                    "[PASS] commit"
run_test "commit persists insert"    "[PASS] commit persists insert"

run_test "Close drops connection"  "[PASS] Close drops connection"

echo ""
echo "────────────────────────────────────────────"
echo -e "  ${GREEN}PASS: $PASS${NC}  |  ${RED}FAIL: $FAIL${NC}  |  ${YELLOW}SKIP: $SKIP${NC}"
echo "────────────────────────────────────────────"
echo ""

[ $FAIL -eq 0 ] && exit 0 || exit 1
