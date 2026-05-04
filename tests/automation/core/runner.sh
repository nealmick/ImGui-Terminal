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

# Detect platform once. uname on MSYS2/MinGW returns MINGW64_NT-*,
# MSYS_NT-*, etc. — anything containing "MINGW" or "MSYS" is Windows.
_uname=$(uname -s)
case "$_uname" in
	MINGW*|MSYS*|CYGWIN*) _is_win=1 ;;
	*)                     _is_win=0 ;;
esac

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
	# Platform-specific baselines: prefer expected_win.json on Windows
	# if it exists, otherwise fall back to expected.json (unified).
	if [ "$_is_win" = "1" ] && [ -f "$tdir/expected_win.json" ]; then
		expected="$tdir/expected_win.json"
	else
		expected="$tdir/expected.json"
	fi

	if [ "$_is_win" = "1" ]; then
		"$client" --input "$tdir/input.sh" --output "$actual" 2>/dev/null
	else
		"$client" --input "$tdir/input.sh" --output "$actual" >/dev/null 2>&1
	fi

	if [ ! -f "$expected" ]; then
		if [ "$update" = "1" ] && [ -f "$actual" ]; then
			create_target="$expected"
			[ "$_is_win" = "1" ] && create_target="$tdir/expected_win.json"
			cp "$actual" "$create_target"
			printf '\033[32mCREATED\033[0m  %s — %s written\n' "$name" "$(basename "$create_target")"
			pass=$((pass + 1))
		else
			printf '\033[33mMISSING\033[0m  %s — no %s yet\n' "$name" "$(basename "$expected")"
			missing=$((missing + 1))
		fi
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
			# On Windows, always write to expected_win.json to
			# avoid clobbering the Linux baseline.
			update_target="$expected"
			[ "$_is_win" = "1" ] && update_target="$tdir/expected_win.json"
			cp "$actual" "$update_target"
			printf '         updated %s\n' "$(basename "$update_target")"
		fi
	fi
done

echo
printf 'pass:%d  fail:%d  missing:%d\n' "$pass" "$fail" "$missing"
[ -n "${SUMMARY_FILE:-}" ] && \
	printf 'pass:%d  fail:%d  missing:%d\n' "$pass" "$fail" "$missing" > "$SUMMARY_FILE"

if [ "$fail" -gt 0 ] && [ "$update" = "0" ]; then
	echo
	echo "to inspect a failure:"
	for n in "${fails[@]}"; do
		printf '  diff %s/%s/expected.json %s/%s/actual.json\n' \
		    "$dir" "$n" "$dir" "$n"
	done
fi

exit $((fail + missing))
