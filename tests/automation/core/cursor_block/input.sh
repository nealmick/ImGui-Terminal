#!/bin/bash
# DECSCUSR \e[2 q — steady block cursor. The default; rendered via
# imw_drawglyph (block uses g.bg via the glyph render, not a separate
# rect). Overlay should contain a bg rect + the underlying glyph.
printf '\e[2 q'
printf 'X'
