#!/bin/bash
# tests/palette-test.sh — manual runtime palette mutation check.
#
# Run inside the terminal under test. Exercises OSC 4 (set color) and
# OSC 104 (reset color), which route through xsetcolorname in the
# adapter. Verifies that runtime palette changes are picked up by
# subsequent SGR color uses, and that resetting restores the original.
#
# OSC sequences exercised:
#   OSC 4 ; <idx> ; <spec> BEL    — set palette entry at <idx>
#   OSC 104 ; <idx> BEL           — reset palette entry at <idx>
#   OSC 104 BEL                   — reset entire palette (no idx arg)
#
# Spec formats accepted (handled by imw_parse_color):
#   #rrggbb   #rgb   #rrrrggggbbbb
#   rgb:R/G/B
#   X11 color names from rgb.txt (e.g. "red", "cornflowerblue")
#
# Palette indices: 0-15 are the ANSI 16, 16-231 are the 6×6×6 cube,
# 232-255 are the grayscale ramp, 256-259 are extended slots
# (cs/rcs/fg/bg).

hr()    { printf '\033[7m=== %-72s ===\033[0m\n' "$1"; }
note()  { printf '\033[2m  (%s)\033[0m\n' "$1"; }
prompt() { printf '\033[2m  press Enter to continue...\033[0m'; read -r; }
osc()   { printf '\033]%s\007' "$1"; }
swatch() { printf "\033[3${1}m sample text on color $1 \033[0m\n"; }

cleanup() { osc '104'; }   # reset entire palette on exit
trap cleanup EXIT INT

hr "Baseline — all 8 ANSI fg colors"
for i in 0 1 2 3 4 5 6 7; do swatch $i; done
note "expect: standard ANSI 0-7 (black, red, green, yellow, blue, magenta, cyan, white)"
prompt

hr "Mutate slot 1 (red) -> bright cyan via #hex"
osc '4;1;#00ffff'
swatch 1
note "the line above should render in bright cyan, not red"
prompt

hr "Mutate slot 2 (green) -> orange via X11 name"
osc '4;2;orange'
swatch 2
note "the line above should render in orange"
note "if not, rgb.txt name lookup or imw_parse_color path failed"
prompt

hr "Mutate slot 4 (blue) -> via rgb: format"
osc '4;4;rgb:ff/80/00'
swatch 4
note "the line above should render in red-orange (255, 128, 0)"
prompt

hr "Reset slot 1 individually (OSC 104 ; 1)"
osc '104;1'
swatch 1
note "slot 1 should be back to default red"
note "slots 2 and 4 should still be the mutated colors"
swatch 2; swatch 4
prompt

hr "Reset entire palette (OSC 104 with no arg)"
osc '104'
for i in 0 1 2 3 4 5 6 7; do swatch $i; done
note "all 8 slots should be back to defaults"
prompt

hr "Bad input — should fail silently"
osc '4;1;not-a-real-color'
swatch 1
note "slot 1 should remain at whatever it was; no crash"
prompt

echo
echo "----- end of palette-test.sh -----"
