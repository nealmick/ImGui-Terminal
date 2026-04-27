#!/bin/bash
# tests/render-test.sh — manual rendering regression check for st adapters.
#
# Run from inside the terminal under test. Output should look identical
# under both the x.c adapter and the imgui_win.cpp adapter — that's the
# "same input → same buffer state → same output" guarantee.
#
# Sections gate on the imgui_win attribute pipeline phases. When
# something regresses, the section header tells you which phase to
# suspect.
#
# Idempotent — one pass, no loops, no sleeps. Scrollable from the top.

hr()   { printf '\033[7m=== %-72s ===\033[0m\n' "$1"; }
note() { printf '\033[2m  (%s)\033[0m\n' "$1"; }

# ----- Phase 1 + 3: indexed colors + truecolor -----

hr "Phase 1 — indexed fg (SGR 30-37 / 90-97)"
for i in 0 1 2 3 4 5 6 7; do printf "\033[3${i}m FG${i} \033[0m"; done
printf "  "
for i in 0 1 2 3 4 5 6 7; do printf "\033[9${i}m FG${i}+8 \033[0m"; done
echo
note "16 distinct fg colors, ANSI 0-7 then bright 8-15"

hr "Phase 1 — indexed bg (SGR 40-47 / 100-107)"
for i in 0 1 2 3 4 5 6 7; do printf "\033[4${i}m BG${i} \033[0m"; done
printf "  "
for i in 0 1 2 3 4 5 6 7; do printf "\033[10${i}m BG${i}+8 \033[0m"; done
echo

hr "Phase 1 — 256-color cube + grayscale ramp (SGR 38;5;N / 48;5;N)"
for i in $(seq 0 255); do
	printf "\033[48;5;${i}m %3d \033[0m" $i
	[ $((i % 16)) -eq 15 ] && echo
done
note "0-15 ANSI; 16-231 6×6×6 cube; 232-255 grayscale ramp"

hr "Phase 3 — truecolor (SGR 38;2;R;G;B / 48;2;R;G;B)"
printf "\033[38;2;255;128;0morange fg\033[0m  "
printf "\033[48;2;20;30;60;38;2;255;200;80mwarm-on-cool\033[0m  "
printf "\033[38;2;255;0;255mmagenta\033[0m\n"
note "exact RGB rendering — should not snap to indexed palette"

# ----- Phase 2: per-cell ATTR_REVERSE -----

hr "Phase 2 — ATTR_REVERSE (SGR 7)"
printf "\033[7mreversed text\033[0m  vs  normal text\n"
printf "\033[31m\033[7mreversed red\033[0m  vs  \033[31mnormal red\033[0m\n"
note "reversed: bg-color text on fg-color bg, per-cell"

# ----- Phase 4: bold + italic via real font variants -----

hr "Phase 4 — bold + italic (SGR 1, 3, 1;3)"
printf "\033[1mBOLD\033[0m  \033[3mITALIC\033[0m  \033[1;3mBOLD-ITALIC\033[0m  normal\n"
note "real font variants, not synthesis — bold heavier, italic actually slanted glyphs"

# ----- Phase 5: faint dimming -----

hr "Phase 5 — ATTR_FAINT (SGR 2)"
printf "\033[2mfaint text\033[0m  vs  normal text\n"
printf "\033[2;31mfaint red\033[0m  vs  \033[31mnormal red\033[0m\n"
printf "\033[1mbold\033[0m  vs  \033[2mfaint\033[0m  vs  \033[1;2mbold+faint (bold wins)\033[0m\n"
note "faint ≈ 50% intensity of normal; BOLD+FAINT → bold (faint is ignored)"

# ----- Phase 6: blink -----

hr "Phase 6 — ATTR_BLINK + MODE_BLINK (SGR 5)"
printf "\033[5mthis should blink\033[0m  vs  steady text\n"
note "toggles every ~800ms — fg goes invisible during the off-phase"

# ----- Phase 7: invisible / underline / strikethrough -----

hr "Phase 7 — ATTR_INVISIBLE / UNDERLINE / STRUCK (SGR 8, 4, 9)"
printf "\033[4munderlined\033[0m  \033[9mstrikethrough\033[0m  \033[8minvisible\033[0m_marker\n"
printf "\033[4;9mboth at once\033[0m\n"
note "underline below baseline; strikethrough at 2/3 ascent"
note "invisible: 'invisible' text is fg=bg, '_marker' should be readable"

# ----- Phase 8: incremental coverage -----

hr "Phase 8a — bold-color promotion (indexed [0,7] + bold → bright)"
printf "plain red:    \033[31mred text\033[0m\n"
printf "bold red:     \033[1;31mred text\033[0m  ← should look BRIGHTER (color 9, not 1)\n"
printf "bold+faint:   \033[1;2;31mred text\033[0m  ← faint wins (no promotion, no halving)\n"

hr "Phase 8b — badweight/badslant override"
note "only triggers when fontconfig falls back to a non-matching variant"
note "current default fonts have all 4 variants → won't fire on this system"
note "would render as defaultattr (yellow) when triggered"

hr "Phase 8c — MODE_REVERSE whole-screen (DECSCNM)"
note "screen will invert for 2s — default bg/fg slots swap, others XOR-invert"
printf "this line uses default fg/bg — should swap cleanly\n"
printf "\033[31mthis line uses explicit red — channels should XOR-invert\033[0m\n"
printf "\033[38;2;255;200;80;48;2;20;30;60mtruecolor — channels XOR-invert\033[0m\n"
printf '\033[?5h'
sleep 2
printf '\033[?5l'
note "back to normal — verify nothing visually stuck (if anything is still"
note "inverted, MODE_REVERSE state-tracking has a bug)"

hr "Phase 8d — border clearing at canvas edges"
note "visible when grid doesn't fit canvas evenly — resize window so the right"
note "or bottom remainder is < cw or ch wide; should be filled with bg color"

echo
echo "----- end of render-test.sh -----"
echo "compare visual output against the x.c adapter to spot divergence."
