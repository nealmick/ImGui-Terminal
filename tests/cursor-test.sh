#!/bin/bash
# tests/cursor-test.sh — manual cursor-style verification.
#
# Run inside the terminal under test. Cycles through the seven cursor
# styles st supports via DECSCUSR (`\033[N q`). For each style, prompts
# you to look at the cursor (it's wherever the shell left it after the
# `read` prompt) and press Enter to advance.
#
# Also exercises focused vs unfocused rendering — focus-gate the terminal
# window before pressing Enter at any prompt, then click outside to lose
# focus and verify the cursor switches between solid (focused) and
# hollow-outline (unfocused).
#
# Each style maps to one of the four shapes × focused/unfocused matrix
# in xdrawcursor:
#   0, 1  — blinking block         (default + explicit blinking)
#   2     — steady block
#   3     — blinking underline
#   4     — steady underline
#   5     — blinking bar (vertical |)
#   6     — steady bar
#   7     — st extension: snowman ☃ (only the focused variant)

hr()   { printf '\033[7m=== %-72s ===\033[0m\n' "$1"; }
note() { printf '\033[2m  (%s)\033[0m\n' "$1"; }
prompt() { printf '\033[2m  press Enter to continue...\033[0m'; read -r; }

cleanup() { printf '\033[0 q'; }   # reset to default on exit / Ctrl+C
trap cleanup EXIT INT

hr "Style 0 / 1 — Blinking block"
printf '\033[1 q'
note "block-shaped cursor, blinks ~every 800ms"
note "click outside the window: cursor should switch to a hollow outline"
prompt

hr "Style 2 — Steady block"
printf '\033[2 q'
note "block-shaped cursor, no blink"
prompt

hr "Style 3 — Blinking underline"
printf '\033[3 q'
note "underscore at cell baseline, blinks"
prompt

hr "Style 4 — Steady underline"
printf '\033[4 q'
note "underscore at cell baseline, no blink"
prompt

hr "Style 5 — Blinking bar"
printf '\033[5 q'
note "thin vertical bar at left edge of cell, blinks"
prompt

hr "Style 6 — Steady bar"
printf '\033[6 q'
note "thin vertical bar at left edge of cell, no blink"
prompt

hr "Style 7 — st snowman extension"
# Style 7 isn't reachable via DECSCUSR; xsetcursor accepts it directly
# but there's no standard escape to set it. Triggered here by writing
# directly to the terminal's xsetcursor — the imgui adapter range-checks
# 0..7. Skip if the terminal rejects it.
printf '\033[7 q' 2>/dev/null
note "should render U+2603 (☃) at cursor position when focused"
note "if unsupported by your terminal, this looks the same as style 6"
prompt

hr "Reset"
printf '\033[0 q'
note "back to default (blinking block); exit code 0"

echo
echo "----- end of cursor-test.sh -----"
