#!/bin/bash
# tests/mouse-test.sh — manual mouse-reporting protocol verification.
#
# Run inside the terminal under test. Enables each mouse-reporting mode
# in turn and pipes raw stdin through `cat -v` so the protocol bytes
# come out as readable text (^[ for ESC, ^M for 0x0D, etc.). Click/drag/
# scroll over the canvas and watch the bytes appear.
#
# Press q + Enter (or Ctrl+C) to advance to the next mode. The cleanup
# trap turns all modes off on exit even if you exit mid-test.
#
# Modes exercised (DECSET — see imgui_win.cpp's mouse-report dispatch):
#   1000  VT200 / MODE_MOUSEBTN   — press + release
#   1002  +MODE_MOUSEMOTION       — adds drag motion while button held
#   1003  +MODE_MOUSEMANY         — adds free motion (no button held)
#   1006  +MODE_MOUSESGR          — text-format encoding (no 223 cell cap)
#
# Default 6-byte format:  ESC [ M <btn+32> <col+32> <row+32>
# SGR (1006) format:      ESC [ < <btn> ; <col> ; <row> M  (M=press, m=release)

cleanup() {
	# Disable every mode, sane terminal state, restore line discipline.
	printf '\033[?1000l\033[?1002l\033[?1003l\033[?1006l'
	stty sane 2>/dev/null
	echo
	echo "----- mouse modes restored -----"
}
trap cleanup EXIT INT

hr() { printf '\033[7m=== %-72s ===\033[0m\n' "$1"; }

run_mode() {
	local name=$1
	local set=$2
	local instr=$3

	echo
	hr "$name (DECSET $set)"
	echo "  Action: $instr"
	echo "  q + Enter (or Ctrl+C) to advance."
	echo

	# Enable + capture. cat -v renders control bytes readably.
	printf '\033[?%sh' "$set"
	stty -icanon -echo 2>/dev/null
	while IFS= read -r -n1 c; do
		[ "$c" = "q" ] && break
		printf '%s' "$c" | cat -v
	done
	stty sane 2>/dev/null
	printf '\033[?%sl' "$set"

	echo
}

cat <<'EOF'
mouse-test.sh — watch the bytes your clicks generate.

Each section enables one mouse-reporting mode. Click/drag/scroll over the
terminal and the bytes the terminal sends to the program appear below.
Type `q` then Enter to advance.

Reading the output:
  ^[          ESC (0x1B)
  ^[[M        VT200/default format header — next 3 bytes encode button, col, row
  ^[[<        SGR format header (mode 1006) — params follow as decimal text
EOF

run_mode "VT200 — press/release only" "1000" \
    "click somewhere — should see ^[[M followed by 3 bytes (btn, col, row)"

run_mode "VT200 + motion-while-pressed" "1000;1002" \
    "click and drag — should see press, motion stream, release"

run_mode "VT200 + free motion" "1000;1003" \
    "just move the cursor (no button held) — should see motion bytes"

run_mode "SGR encoding" "1000;1006" \
    "click — should see ^[[<<btn>;<col>;<row>M  (or m for release)"

echo
echo "----- end of mouse-test.sh -----"
