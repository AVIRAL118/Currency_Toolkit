#!/usr/bin/env bash
# tests/test_engine_smoke.sh
# Batch smoke test: one engine process, one command per line,
# compare replies line-by-line against expected.
# Fully compatible with Bash 3.2+ (macOS default) and Bash 4+ (Linux).

set -uo pipefail   # NOT -e: we want to collect all failures, not abort on first.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

ENGINE="${1:-$REPO_ROOT/engine_test}"
SOURCE="$REPO_ROOT/currency_engine.c"

# --- Robust auto-build & stale-binary check ---
#
# Decide whether to (re)build the engine. Source existence is checked
# first so the "source missing, binary missing" case gets a clear error
# instead of a confusing gcc failure.
if [[ ! -f "$SOURCE" ]]; then
    if [[ ! -x "$ENGINE" ]]; then
        echo "ERROR: neither $SOURCE nor $ENGINE found; cannot proceed." >&2
        exit 2
    fi
    echo "⚠️ Warning: $SOURCE not found; using existing $ENGINE without rebuild." >&2
    need_build=0
elif [[ ! -x "$ENGINE" ]]; then
    need_build=1
    reason="binary missing"
elif [[ "$SOURCE" -nt "$ENGINE" ]]; then
    need_build=1
    reason="source is newer than binary"
else
    need_build=0
fi

if [[ "$need_build" -eq 1 ]]; then
    if ! command -v gcc >/dev/null 2>&1; then
        echo "ERROR: gcc not found on PATH; install a C compiler or build manually." >&2
        exit 2
    fi
    echo "⚙️ Rebuilding engine from $SOURCE ($reason)..." >&2
    # Prefer the Makefile so the smoke test and `make` produce identical binaries.
    if [[ -f "$REPO_ROOT/Makefile" ]]; then
        if ! ( cd "$REPO_ROOT" && make engine_test ); then
            echo "ERROR: 'make engine_test' failed (see compiler output above)" >&2
            exit 2
        fi
    else
        if ! gcc -std=c11 -Wall -Wextra -pedantic -O2 "$SOURCE" -o "$ENGINE" -lm; then
            echo "ERROR: auto-build failed (see compiler output above)" >&2
            exit 2
        fi
    fi
fi
# -----------------------------------------------

# Load commands (Bash 3.2 compatible)
CMDS=()
while IFS= read -r line || [[ -n "$line" ]]; do
    CMDS+=("$line")
done <<'EOF'
SET x USD10
EVAL x
EVAL x + 5
SET e EUR5
EVAL x + e
SETN y 7
EVAL y * 2
EVAL 1/0
EVAL x * x
EVAL x / x
RATE USD EUR 0.9
CONVERT x EUR
VARS
QUIT
EOF

# Note on numeric formatting: the engine prints with "%.15g".
# CONVERT x EUR yields exactly "9" because 10 * 0.9 rounds to 9 at
# 15 significant digits. If you change the rate, verify the new expected
# reply with:
#     printf 'RATE USD EUR <rate>\nSET x USD10\nCONVERT x EUR\n' \
#         | ./engine_test | tail -1
EXPECTED=()
while IFS= read -r line || [[ -n "$line" ]]; do
    EXPECTED+=("$line")
done <<'EOF'
OK 10 USD
OK 10 USD
OK 15 USD
OK 5 EUR
ERR currency mismatch in addition/subtraction
OK 7 NONE
OK 14 NONE
ERR division by zero
ERR cannot multiply two currencies
OK 1 NONE
OK RATE
OK 9 EUR
VARLIST x=USD10 e=EUR5 y=7
OK BYE
EOF

if [[ "${#CMDS[@]}" -ne "${#EXPECTED[@]}" ]]; then
    echo "ERROR: ${#CMDS[@]} commands vs ${#EXPECTED[@]} expected replies" >&2
    exit 2
fi

if ! ACTUAL=$(printf '%s\n' "${CMDS[@]}" | "$ENGINE"); then
    echo "ERROR: engine exited non-zero while processing the batch" >&2
    echo "--- raw output ---" >&2
    echo "${ACTUAL:-<empty>}" >&2
    exit 3
fi

ACTUAL_LINES=()
while IFS= read -r line || [[ -n "$line" ]]; do
    ACTUAL_LINES+=("$line")
done <<< "$ACTUAL"

if [[ "${#ACTUAL_LINES[@]}" -ne "${#CMDS[@]}" ]]; then
    echo "ERROR: expected ${#CMDS[@]} replies, got ${#ACTUAL_LINES[@]}" >&2
    echo "--- raw output ---" >&2
    printf '  %s\n' "${ACTUAL_LINES[@]}" >&2
    exit 3
fi

PASS=0
FAIL=0
for i in "${!CMDS[@]}"; do
    want="${EXPECTED[$i]}"
    got="${ACTUAL_LINES[$i]}"
    if [[ "$got" == "$want" ]]; then
        PASS=$((PASS + 1))
    else
        FAIL=$((FAIL + 1))
        echo "FAIL: cmd[$i]=<${CMDS[$i]}>"
        echo "  expected: <$want>"
        echo "  actual:   <$got>"
    fi
done

echo "Passed: $PASS, Failed: $FAIL"
# Exit 0 on success, 1 on any failure — the standard contract for CI.
[[ "$FAIL" -eq 0 ]]