#!/usr/bin/env bash
# Systematic static regression runner for the skate3 source tree.
# Runs every check in tests/ against the current src/ (and the rexglue SDK
# where relevant) WITHOUT building the full project. Intended to be run after
# every source change to catch the class of bug that broke the last build
# (undeclared cvar use) plus dead/redundant references.
#
# Usage:
#     tests/run_tests.sh            # run everything, report pass/fail
#     tests/run_tests.sh --json     # machine-readable summary on stdout
#     tests/run_tests.sh <name>...  # only named checks, e.g. "cvars deadcode"
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

JSON=0
if [[ "${1:-}" == "--json" ]]; then JSON=1; shift; fi

# All runnable checks: <name> <cmd...>
checks=(
  "cvars     python3 tests/check_cvars.py --dir src --sdk third_party/rexglue-sdk"
  "deadcode  python3 tests/check_deadcode.py"
)

# Optional restriction to a subset named on the CLI (after an optional --json).
filter=("$@")

run_one() {
  local name="$1"; shift
  local start end rc
  start=$(date +%s)
  out=$("$@" 2>&1)
  rc=$?
  end=$(date +%s)
  printf "%s\t%s\t%ss\n" "$name" "$([ $rc -eq 0 ] && echo PASS || echo FAIL)" "$((end-start))"
  if [ $rc -ne 0 ]; then
    printf "  --- %s output (first 40 lines) ---\n%s\n  --- end ---\n" "$name" \
      "$(printf '%s' "$out" | head -40)"
  fi
  return $rc
}

overall=0
declare -a results=()
for entry in "${checks[@]}"; do
  name="${entry%% *}"
  cmd="${entry#* }"
  # applying maybe the cmd has quotes; eval it
  if [ ${#filter[@]} -gt 0 ]; then
    keep=0
    for f in "${filter[@]}"; do [ "$f" == "$name" ] && keep=1; done
    [ $keep -eq 0 ] && continue
  fi

  if [ $JSON -eq 1 ]; then
    start=$(date +%s)
    out=$(eval "$cmd" 2>&1); rc=$?
    end=$(date +%s)
    printf '{"name":"%s","status":"%s","seconds":%d}\n' "$name" \
      "$([ $rc -eq 0 ] && echo PASS || echo FAIL)" "$((end-start))"
    [ $rc -ne 0 ] && overall=1
  else
    res=$(run_one "$name" bash -c "$cmd")
    echo "$res"
    case "$res" in *$'\tFAIL\t'*) overall=1;; esac
  fi
done

[ $JSON -eq 0 ] && [ $overall -ne 0 ] && echo "==> SOME CHECKS FAILED" || true
exit $overall
