/* See LICENSE for license details. */
/*
 * imgui_user_config.h — project-level ImGui compile-time options.
 *
 * Loaded via -DIMGUI_USER_CONFIG=... in the Makefile. Keeps the imgui
 * submodule pristine (no edits to imgui/imconfig.h required when updating).
 *
 */

#pragma once

/* IMGUI_ENABLE_FREETYPE — use FreeType as the glyph rasterizer instead of
 * the bundled stb_truetype. Same engine x.c uses (via Xft → FreeType), so
 * glyph quality, hinting, and font discovery semantics match upstream. */
#define IMGUI_ENABLE_FREETYPE

/* IMGUI_USE_WCHAR32 — promote ImWchar to 32-bit (default is 16-bit / BMP).
 * st's Rune is uint_least32_t; without this, codepoints > U+FFFF (emoji,
 * CJK Ext B–G, etc.) are silently truncated at the ImGui API boundary.
 * Mandatory, not optional. */
#define IMGUI_USE_WCHAR32
