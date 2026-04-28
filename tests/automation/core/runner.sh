#!/bin/bash
# tests/automation/core/runner.sh — run every core test, diff result
# against committed expected.json, report pass/fail.
#
# Usage:
#   ./tests/automation/core/runner.sh           # run all tests
#   ./tests/automation/core/runner.sh --update  # overwrite expected.json
#                                               # with actual.json for tests
#                                               # that diverged (review the
#                                               # diff in git first!)

set -u

dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$dir/../../.." && pwd)
client="$repo/build/test_core"

if [ ! -x "$client" ]; then
	echo "test_core not built — run 'make test_core' first" >&2
	exit 2
fi

update=0
[ "${1:-}" = "--update" ] && update=1

pass=0
fail=0
missing=0
fails=()

for tdir in "$dir"/*/; do
	[ -f "$tdir/input.sh" ] || continue

	name=$(basename "$tdir")
	actual="$tdir/actual.json"
	expected="$tdir/expected.json"

	"$client" --input "$tdir/input.sh" --output "$actual" >/dev/null 2>&1

	if [ ! -f "$expected" ]; then
		printf '\033[33mMISSING\033[0m  %s — no expected.json yet\n' "$name"
		printf '         to baseline: cp %s %s\n' "$actual" "$expected"
		missing=$((missing + 1))
		continue
	fi

	if diff -q "$expected" "$actual" >/dev/null 2>&1; then
		printf '\033[32mPASS\033[0m     %s\n' "$name"
		pass=$((pass + 1))
	else
		printf '\033[31mFAIL\033[0m     %s\n' "$name"
		fails+=("$name")
		fail=$((fail + 1))
		if [ "$update" = "1" ]; then
			cp "$actual" "$expected"
			printf '         updated expected.json\n'
		fi
	fi
done

echo
printf 'pass:%d  fail:%d  missing:%d\n' "$pass" "$fail" "$missing"

if [ "$fail" -gt 0 ] && [ "$update" = "0" ]; then
	echo
	echo "to inspect a failure:"
	for n in "${fails[@]}"; do
		printf '  diff %s/%s/expected.json %s/%s/actual.json\n' \
		    "$dir" "$n" "$dir" "$n"
	done
fi

exit $((fail + missing))
