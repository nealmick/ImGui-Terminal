#!/bin/bash
# 24-bit truecolor fg + bg + mixed. Exercises IS_TRUECOL path through
# imw_resolve_glyph_color; the dump should show exact "#rrggbb" values.
printf '\e[38;2;255;0;0mR\e[0m'
printf '\e[38;2;0;255;0mG\e[0m'
printf '\e[38;2;0;0;255mB\e[0m'
printf '\e[38;2;128;64;192mP\e[0m'
printf '\e[38;2;255;255;255;48;2;100;50;200mW\e[0m'
printf '\r\n'
# Mixed: indexed fg with truecolor bg in same cell — forces run-batch break.
printf '\e[31;48;2;0;128;128mA\e[38;2;200;200;0;41mB\e[0m'
