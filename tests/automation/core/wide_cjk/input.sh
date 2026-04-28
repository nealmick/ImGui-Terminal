#!/bin/bash
# Wide CJK characters — each ideograph spans 2 cells. Exercises
# runewidth = 2*cw in the spec packer and the WDUMMY skip in
# imw_makeglyphfontspecs. Dump should show TEXT ops at every-other
# column (no TEXT op for the dummy follow-cell).
printf '中文测试'
printf '\r\n'
printf '日本語'
printf '\r\n'
printf 'a中b文c'
