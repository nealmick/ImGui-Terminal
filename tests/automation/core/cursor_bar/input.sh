#!/bin/bash
# DECSCUSR \e[6 q — steady bar cursor. Rendered as a single thin vertical
# rect at winx with width=cursorthickness via imw_clear. Overlay should
# contain just one RECT op.
printf '\e[6 q'
printf 'X'
