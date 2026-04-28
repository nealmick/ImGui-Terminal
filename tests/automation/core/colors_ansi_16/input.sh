#!/bin/bash
# 16-color ANSI fg + bg. Row 0: 8 normal fg colors. Row 1: 8 bright fg.
# Row 2: 8 normal bg. Row 3: 8 bright bg.
# Catches indexed-palette lookup bugs and the bright vs normal split.
for i in 0 1 2 3 4 5 6 7; do printf '\e[3%dmX' "$i"; done; printf '\e[0m\r\n'
for i in 0 1 2 3 4 5 6 7; do printf '\e[9%dmX' "$i"; done; printf '\e[0m\r\n'
for i in 0 1 2 3 4 5 6 7; do printf '\e[37;4%dmX' "$i"; done; printf '\e[0m\r\n'
for i in 0 1 2 3 4 5 6 7; do printf '\e[37;10%dmX' "$i"; done; printf '\e[0m\r\n'
