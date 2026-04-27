#!/bin/bash
# tests/title-test.sh — manual window-title verification.
#
# Run inside the terminal under test. Cycles through OSC 0/1/2 title
# sequences; each updates the ImGui window's title via xsettitle. Look
# at the title bar after each step and press Enter to advance.
#
# OSC sequences exercised:
#   OSC 0 ; <title> BEL  — set both icon name and window title
#   OSC 1 ; <title> BEL  — set icon name only (we collapse this to title)
#   OSC 2 ; <title> BEL  — set window title only
#
# In the imgui adapter, xseticontitle is intentionally a no-op (ImGui
# windows have no separate icon-name concept), and xsettitle stores the
# string into term_title which gets rendered into ImGui::Begin's label
# via the `###st_term_widget` persistent ID — display label updates,
# window state (size/position/dock) survives the change.

hr()    { printf '\033[7m=== %-72s ===\033[0m\n' "$1"; }
note()  { printf '\033[2m  (%s)\033[0m\n' "$1"; }
prompt() { printf '\033[2m  press Enter to continue...\033[0m'; read -r; }
osc()   { printf '\033]%s\007' "$1"; }   # OSC ... BEL

cleanup() { osc '2;Terminal'; }   # restore default-ish title on exit
trap cleanup EXIT INT

hr "OSC 2 — set window title"
osc '2;hello world'
note "title bar should now read: hello world"
prompt

hr "OSC 0 — set both (icon + title)"
osc '0;both fields'
note "title bar should now read: both fields"
prompt

hr "OSC 1 — set icon name only"
osc '1;icon name'
note "x.c would change icon-title; imgui adapter intentionally ignores"
note "(no separate icon concept in ImGui windows). Title bar should NOT"
note "change from the previous step."
prompt

hr "Title with special characters"
osc $'2;path/to/file — name with spaces & emoji 🎉'
note "title bar should render the full string including emoji"
prompt

hr "Empty title"
osc '2;'
note "title bar should fall back to a default (`Terminal`)"
prompt

hr "Long title"
osc '2;this is a very long terminal title that probably exceeds the visible width of any reasonable window title bar layout'
note "title should be set; renderer/host may truncate visually"
prompt

echo
echo "----- end of title-test.sh -----"
