#!/bin/bash
# OSC 4 palette mutation. Print color 1 (red) before and after redefining
# color 1 to magenta via OSC 4. Both prints end up MAGENTA (#ff00ff) in
# the dump — that's the correct indexed-semantics behavior, matching
# xterm and upstream st: cells store a palette INDEX, so when slot 1's
# RGB changes, every already-rendered cell using slot 1 repaints to the
# new value. Verifies two things together:
#   - imw_setcolorname mutates colors[] (else AFTER would still be red)
#   - redraw-all-rows-on-palette-change re-records existing rows' ops
#     (else BEFORE would still be red)
# If either piece breaks, the diff fails.
printf '\e[41mBEFORE\e[0m'
printf '\r\n'
printf '\e]4;1;rgb:ff/00/ff\e\\'
printf '\e[41mAFTER\e[0m'
