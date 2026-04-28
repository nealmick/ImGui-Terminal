#!/bin/bash
# DECSCUSR \e[4 q — steady underline cursor. Rendered as a single thin
# rect at winy + ch - cursorthickness via imw_clear. Overlay should
# contain just one RECT op for the cursor (plus the old-cursor-erase
# glyph from the move).
printf '\e[4 q'
printf 'X'
