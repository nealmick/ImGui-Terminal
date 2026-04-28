#!/bin/bash
# Attribute combinations. Each cell exercises a different ATTR_* flag,
# which drives the font/color resolution chain in imw_drawglyphfontspecs.
# Bold/italic/bold_italic select different font slots (visible in the
# dump's "font" field). Underline/strike emit thin RECT ops.
printf 'normal '
printf '\e[1mbold\e[0m '
printf '\e[3mitalic\e[0m '
printf '\e[1;3mboth\e[0m '
printf '\e[4munderline\e[0m '
printf '\e[9mstrike\e[0m '
printf '\e[1;3;4;9mall\e[0m'
printf '\r\n'
# Bold-promotion: indexed [0,7] should promote to [8,15] under bold.
# Cell with \e[1;31m should resolve to bright-red (color index 9).
printf '\e[31mNORM\e[0m \e[1;31mBOLD\e[0m'
