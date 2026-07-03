#!/bin/bash
# tests/char-test.sh — non-ASCII / wide-char / box-drawing rendering check.
#
# Companion to render-test.sh (which covers the attribute pipeline). This
# script focuses on character-coverage and ATTR_WIDE behavior — useful to
# run after font / atlas / FRC changes to spot regressions without wading
# through unrelated phase tests.
#
# Run from the repo root, same as render-test.sh.

hr()   { printf '\033[7m=== %-72s ===\033[0m\n' "$1"; }
note() { printf '\033[2m  (%s)\033[0m\n' "$1"; }

hr "UTF-8 basic — Latin extended"
printf "naïve résumé crème brûlée  Æ ø Ñ ß  Ω ƒ µ\n"
note "common diacritics; no replacement boxes expected"

hr "UTF-8 — IPA / phonetic"
printf "/ˈtɛɹmɪnəl/  /θ/ /ð/ /ʃ/ /ʒ/ /ŋ/ /æ/ /ɑ/\n"
note "depends on font's IPA coverage"

hr "UTF-8 — math + science"
printf "∀x∈ℝ : x² ≥ 0   π ≈ 3.14159   √2 = 1.414…   ∞   ∂   ∇\n"
printf "∑   ∏   ∫   ≠ ≤ ≥ ≈   ⊂ ⊃ ∈ ∉   ∧ ∨ ¬\n"

hr "UTF-8 — arrows"
printf "← → ↑ ↓ ↔ ↕   ⇒ ⇐ ⇔   ⟶ ⟵ ⟷   ⤴ ⤵   ↻ ↺\n"

hr "UTF-8 — misc symbols"
printf "• ★ ☃ ✓ ✗ ❯ ▲ ▼ ◆ ♠ ♣ ♥ ♦ © ® ™\n"

hr "Box-drawing — light"
printf "─ │ ┌ ┐ └ ┘ ├ ┤ ┬ ┴ ┼\n"
printf "  ┌─────────┬─────────┐\n"
printf "  │ cell A  │ cell B  │\n"
printf "  ├─────────┼─────────┤\n"
printf "  │ cell C  │ cell D  │\n"
printf "  └─────────┴─────────┘\n"
note "corners must connect seamlessly — no gaps or overlap"

hr "Box-drawing — heavy + double + curved"
printf "Heavy:    ━ ┃ ┏ ┓ ┗ ┛ ┣ ┫ ┳ ┻ ╋\n"
printf "Double:   ═ ║ ╔ ╗ ╚ ╝ ╠ ╣ ╦ ╩ ╬\n"
printf "Curved:   ╭ ╮ ╰ ╯\n"

hr "Block elements — shading + bar gradients"
printf "Shades:    ░ ▒ ▓ █\n"
printf "Bars 1/8:  ▏ ▎ ▍ ▌ ▋ ▊ ▉ █\n"
printf "Halves:    ▀ ▄ ▌ ▐\n"
note "useful for progress bars / sparklines — flush with neighbors"

hr "Wide characters — CJK alignment test (ATTR_WIDE)"
printf "Chinese:   中文测试 — each ideograph spans 2 cells\n"
printf "Japanese:  こんにちは 日本語 テスト\n"
printf "Korean:    안녕하세요 한국어\n"
echo
note "alignment grid (3 wide chars per row, columns must line up):"
printf "    あいう\n"
printf "    かきく\n"
printf "    さしす\n"
note "if columns don't line up, ATTR_WIDE handling is broken"
note "if a glyph overlaps the next cell, runewidth=2*cw isn't propagating"

hr "Emoji — color rendering (ImGuiFreeTypeLoaderFlags_LoadColor)"
printf "Faces:    😀 😎 🤔 😴 🥳 🤖\n"
printf "Symbols:  🔴 🟢 🔵 ⭐ 🎉 ✨\n"
printf "Tools:    🔧 🔨 💻 📦 🐛 🚀\n"
printf "Width:    |A☕B| |A🖐️B| — B must start two cells after each emoji\n"
note "color emoji should render in color (not as replacement glyphs)"
note "via fontconfig fallback merge — Apple Color Emoji on macOS, Noto on Linux"

hr "Combining marks (decomposed grapheme clusters)"
printf "decomposed: a\\u0301 e\\u0302 n\\u0303  →  á ê ñ\n"
printf "complex:    👨‍👩‍👧 family ZWJ sequence\n"
note "these are HARD; partial support is normal"

echo
echo "----- end of char-test.sh -----"
