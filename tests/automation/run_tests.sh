#!/bin/bash
# tests/automation/run_tests.sh — run both automation suites (core +
# input) and print a combined pass/fail total.
#
# Each per-suite runner is also runnable on its own:
#   ./tests/automation/core/runner.sh    # only core tests
#   ./tests/automation/input/runner.sh   # only input tests
#
# Usage:
#   ./tests/automation/run_tests.sh           # run all
#   ./tests/automation/run_tests.sh --update  # overwrite expected.json
#                                             # for any failure (review
#                                             # the diff in git first!)

set -u

dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)

core_log=$(mktemp)
input_log=$(mktemp)
trap 'rm -f "$core_log" "$input_log"' EXIT

printf '\033[1;34m== core ==\033[0m\n'
"$dir/core/runner.sh" "$@" 2>&1 | tee "$core_log"
rc_core=${PIPESTATUS[0]}

echo
printf '\033[1;34m== input ==\033[0m\n'
"$dir/input/runner.sh" "$@" 2>&1 | tee "$input_log"
rc_input=${PIPESTATUS[0]}

# Pull the "pass:N  fail:N  missing:N" line out of each log. If a suite
# crashed before printing the summary the field defaults to 0 — the
# non-zero exit code from the runner still propagates below.
extract() {
	local log=$1 field=$2
	grep -oE "${field}:[0-9]+" "$log" | tail -1 | grep -oE '[0-9]+'
}

core_pass=$(extract "$core_log" pass);    core_pass=${core_pass:-0}
core_fail=$(extract "$core_log" fail);    core_fail=${core_fail:-0}
core_miss=$(extract "$core_log" missing); core_miss=${core_miss:-0}
in_pass=$(extract "$input_log" pass);     in_pass=${in_pass:-0}
in_fail=$(extract "$input_log" fail);     in_fail=${in_fail:-0}
in_miss=$(extract "$input_log" missing);  in_miss=${in_miss:-0}

echo
printf '\033[1;34m== total ==\033[0m\n'
printf 'pass:%d  fail:%d  missing:%d\n' \
    $((core_pass + in_pass)) \
    $((core_fail + in_fail)) \
    $((core_miss + in_miss))

exit $((rc_core + rc_input))
