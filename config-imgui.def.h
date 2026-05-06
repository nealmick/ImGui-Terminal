/* See LICENSE for license details. */
/*
 * config-imgui.def.h — adapter-side config for the ImGui windowing layer.
 *
 * Mirror of config.def.h for items consumed by imgui_win.cpp. Keep the
 * non-input items in sync with upstream config.def.h when patches land.
 *
 * input-config (key[], shortcuts[], mshortcuts[], selmasks[],
 * forcemousemod, ignoremod) lives here in ImGui-native types
 * (ImGuiKey, ImGuiMod_*, ImGuiMouseButton_*) — that's the honest
 * divergence from config.def.h. Everything else here mirrors verbatim.
 *
 * ============================================================
 * SINGLE-TRANSLATION-UNIT INCLUDE RULE — DO NOT VIOLATE.
 * ============================================================
 * This file (and its generated copy config-imgui.h) defines STORAGE for
 * the non-static globals declared as `extern` in st.h (utmp, tabspaces,
 * worddelimiters, etc.). Including it from a second .cpp / .c file
 * produces multiple-definition link errors. Upstream config.def.h has
 * the same property and avoids the issue by convention: only x.c includes
 * it. We follow the same rule: ONLY imgui_win.cpp includes config-imgui.h.
 * If imgui_win.cpp is ever split into multiple files, the storage
 * definitions need to move to a single dedicated .c/.cpp TU and the
 * header itself becomes externs-only. Until then, stay disciplined.
 *
 * No header guards (matches config.def.h's convention — duplicate include
 * is a duplicate-symbol error, which is the right failure mode and the
 * fastest way to catch a violation of the single-TU rule).
 *
 * UPSTREAM DRIFT NOTE: items below mirror config.def.h verbatim. There is
 * no automated sync — when pulling upstream st changes, do
 *     diff config.def.h config-imgui.def.h
 * to spot drift in shared items (fonts, colors, timeouts, behavior flags).
 * The input arrays at the bottom are honest divergence — they live in
 * a different namespace (ImGui types) and don't need to be diffed.
 */

/* Self-contained dependencies — config.def.h relies on x.c having
 * pulled these in already; we include them ourselves so this file can
 * be tooled / opened standalone without spurious "NULL undefined" errors. */
#include <stddef.h>   /* NULL */
#include <wchar.h>    /* wchar_t — built-in in C++, typedef'd in C */

/* ---------- Font / cell sizing — mirrors config.def.h:8, 29-30 ---------- */

#ifdef _WIN32
static const char *font =
    "Consolas:pixelsize=16:antialias=true:autohint=true";
#else
static const char *font =
    "Menlo:pixelsize=16:antialias=true:autohint=true";
#endif

static const float cwscale = 1.0f;
static const float chscale = 1.0f;

/* ---------- Border + cursor — mirrors config.def.h:9, 68 ---------- */

static int          borderpx        = 2;
static unsigned int cursorthickness = 2;

/* ---------- Blink timing — mirrors config.def.h:63 ---------- */
/* Period in ms between MODE_BLINK toggles. Same flag drives cell-level
 * ATTR_BLINK visibility and (eventually) cursor blink — single timer. */
static unsigned int blinktimeout = 800;

/* ---------- st.c-visible globals (declared as externs in st.h) ----------
 * These are the non-static items from config.def.h. They must have C
 * linkage so st.c's `extern` declarations resolve. */

#ifdef __cplusplus
extern "C" {
#endif

char         *utmp             = NULL;
char         *scroll           = NULL;
char         *stty_args        = (char *)"stty raw pass8 nl -echo -iexten -cstopb 38400";
char         *vtiden           = (char *)"\033[?6c";
wchar_t      *worddelimiters   = (wchar_t *)L" ";
int           allowaltscreen   = 1;
int           allowwindowops   = 0;
char         *termname         = (char *)"st-256color";
unsigned int  tabspaces        = 8;

/* Default color palette indices — mirror config.def.h:132-134 */
unsigned int  defaultfg        = 258;
unsigned int  defaultbg        = 259;
unsigned int  defaultcs        = 256;

#ifdef __cplusplus
}
#endif

/* ---------- Adapter-only statics — mirror config.def.h:135, 164 ---------- */

static unsigned int defaultrcs  = 257;  /* config.def.h:135 */
static unsigned int defaultattr = 11;   /* config.def.h:164 */

/* ---------- Default colorname[] — mirrors config.def.h:97-125 ----------
 * config.def.h uses C99 designated initializers (`[255] = 0` etc.) to
 * express a sparse array. C++11 doesn't support designated array
 * initializers, so we split into two dense tables. Semantically identical:
 * indices 0-15 use colorname_low; 16-255 derive from the 256-color cube /
 * grayscale ramp algorithmically (see imw_resolve_color_at in imgui_win.cpp);
 * 256-259 use colorname_high.
 *
 * Patches that touch the upstream colorname[] need to be applied to one
 * of these two arrays here — the structural split is the only divergence;
 * the actual color names match config.def.h verbatim. */

static const char *colorname_low[16] = {
	/* 8 normal colors */
	"black",
	"red3",
	"green3",
	"yellow3",
	"blue2",
	"magenta3",
	"cyan3",
	"gray90",

	/* 8 bright colors */
	"gray50",
	"red",
	"green",
	"yellow",
	"#5c5cff",
	"magenta",
	"cyan",
	"white",
};

static const char *colorname_high[4] = {
	"#cccccc",   /* 256 — defaultcs  */
	"#555555",   /* 257 — defaultrcs */
	"gray90",    /* 258 — defaultfg  */
	"black",     /* 259 — defaultbg  */
};

/* ---------- Input bindings ------------
 *
 * Same shape as upstream config.def.h's input section, just with ImGui
 * constants in the cells. The Shortcut / MouseShortcut / Key types are
 * defined in imgui_win.cpp ABOVE the include of this file (mirroring x.c).
 *
 * Mod constants (combine via |):
 *   ImGuiMod_Ctrl   ImGuiMod_Shift   ImGuiMod_Alt   ImGuiMod_Super
 *   IMW_MOD_ANY  — match any modifier state (x.c's XK_ANY_MOD)
 *   TERMMOD      — conventional st prefix (Ctrl + Shift)
 *
 * Mouse buttons:
 *   ImGuiMouseButton_Left / Right / Middle (= 0 / 1 / 2)
 *   IMW_MB_WHEELUP / IMW_MB_WHEELDOWN — synthetic button values for
 *   wheel events. ImGui exposes scroll as io.MouseWheel (axis), not
 *   a button; the dispatch in imgui_win.cpp turns nonzero wheel
 *   delta into a synthesized press of these buttons that walks
 *   mshortcuts[] like any other click.
 */

/* Modifier that, when held during mouse input, forces selection / shortcut
 * routing rather than mouse-protocol reporting. Mirrors config.def.h:171. */
static int forcemousemod = ImGuiMod_Shift;

/* Modifiers stripped from `state` before shortcut/keymap matching.
 * Default 0; on systems where NumLock/CapsLock should be ignored, set
 * to (ImGuiMod_*) for those locks (note: ImGui exposes these via
 * io.KeyMods on platforms that report them). */
static int ignoremod = 0;

/* Mouse-shortcut table — config.def.h:172-185 mirror.
 *
 * Order matters: first-match wins (mirrors x.c::mouseaction). The Shift
 * variants must come BEFORE the IMW_MOD_ANY catch-all, otherwise
 * Shift-modified wheel scrolls would match the catch-all and never
 * reach the Shift entry.
 *
 * Wheel bindings mirror config.def.h:180-183:
 *   wheel-up        → \031 = Ctrl+Y (less: scroll up 1 line)
 *   wheel-down      → \005 = Ctrl+E (less: scroll down 1 line)
 *   Shift+wheel-up  → \033[5;2~ = Shift+PageUp (less: page up)
 *   Shift+wheel-down→ \033[6;2~ = Shift+PageDown (less: page down)
 *
 * `release=0` for wheel — no release edge for an axis event (the
 * wheel synthesis fires once per nonzero io.MouseWheel frame). */
static MouseShortcut mshortcuts[] = {
	/* mods            button                    function  arg                 release */
	{ IMW_MOD_ANY,     ImGuiMouseButton_Middle,  selpaste, {0},                1 },
	{ ImGuiMod_Shift,  IMW_MB_WHEELUP,           ttysend,  {.s = "\033[5;2~"}, 0 },
	{ IMW_MOD_ANY,     IMW_MB_WHEELUP,           ttysend,  {.s = "\031"},      0 },
	{ ImGuiMod_Shift,  IMW_MB_WHEELDOWN,         ttysend,  {.s = "\033[6;2~"}, 0 },
	{ IMW_MOD_ANY,     IMW_MB_WHEELDOWN,         ttysend,  {.s = "\005"},      0 },
};

/* Keyboard shortcut table — config.def.h:187-204 mirror.
 *
 * Notes on what's omitted vs upstream:
 *
 * 1. Zoom entries (Ctrl+Shift+PageUp/PageDown/Home → zoom/zoomabs/zoomreset)
 *    use `float` Arg fields which require C++20 designated-init-for-union
 *    to express in a static array under -std=c++11. They land back when
 *    font-reload-on-zoom is implemented.
 *
 * 2. The `XK_ANY_MOD, XK_Print, printsel` upstream entry is omitted: this
 *    table is dispatched via ImGui::Shortcut which requires a specific
 *    chord — there's no "any mod" sentinel for chords. IMW_MOD_ANY in
 *    shortcuts[] would degenerate to an all-bits-set chord that never
 *    matches. Use a specific chord here (e.g., add Alt+PrintScreen) or
 *    move "any-mod" semantics to key[] which uses IsKeyPressed +
 *    imw_match_mods (where IMW_MOD_ANY works correctly).
 */
static Shortcut shortcuts[] = {
	/* chord                                       function       arg          */
	{ ImGuiKey_PrintScreen | ImGuiMod_Ctrl,        toggleprinter, {0}          },
	{ ImGuiKey_PrintScreen | ImGuiMod_Shift,       printscreen,   {0}          },
	{ ImGuiKey_C           | TERMMOD,              clipcopy,      {0}          },
	{ ImGuiKey_V           | TERMMOD,              clippaste,     {0}          },
	{ ImGuiKey_Y           | TERMMOD,              selpaste,      {0}          },
	{ ImGuiKey_Insert      | ImGuiMod_Shift,       selpaste,      {0}          },
	{ ImGuiKey_NumLock     | TERMMOD,              numlock,       {0}          },
};

/* Selection-mask table — indexed by SEL_* type (st.h enum). Index 0 is
 * unused since SEL_* enum values start at 1. SEL_REGULAR (no mods) and
 * SEL_RECTANGULAR (Alt held during drag). Mirrors config.def.h:464. */
static int selmasks[] = {
	0,                  /* [0] unused */
	0,                  /* [1] SEL_REGULAR */
	ImGuiMod_Alt,       /* [2] SEL_RECTANGULAR */
};

/* Special-key dispatch table — config.def.h:243-454 mirror. Compact
 * starter set: cursor keys (with appcursor variants), Tab/Backspace/
 * Escape/Enter, Home/End/PageUp/PageDown/Insert/Delete, F1-F12.
 * Extend as terminal apps reveal missing bindings.
 *
 * appkey:    -1 = match only when MODE_APPKEYPAD NOT set
 *             0 = match in any keypad mode (indifferent)
 *            +1 = match only when MODE_APPKEYPAD set
 *            +2 = numlock-paired (skipped when MODE_NUMLOCK set)
 * appcursor: same three-valued logic, against MODE_APPCURSOR.
 */
static Key key[] = {
	/* keysym               mods             string       appkey appcursor */
	{ ImGuiKey_Tab,         ImGuiMod_Shift,  "\033[Z",    0,     0 },
	{ ImGuiKey_Tab,         IMW_MOD_ANY,     "\t",        0,     0 },
	{ ImGuiKey_Backspace,   IMW_MOD_ANY,     "\177",      0,     0 },
	{ ImGuiKey_Escape,      IMW_MOD_ANY,     "\033",      0,     0 },
	{ ImGuiKey_Enter,       IMW_MOD_ANY,     "\r",        0,     0 },

	/* Cursor keys: appcursor=-1 → "\033[X" in normal mode;
	 *              appcursor=+1 → "\033OX" in application-cursor mode. */
	{ ImGuiKey_UpArrow,     IMW_MOD_ANY,     "\033[A",    0,    -1 },
	{ ImGuiKey_UpArrow,     IMW_MOD_ANY,     "\033OA",    0,    +1 },
	{ ImGuiKey_DownArrow,   IMW_MOD_ANY,     "\033[B",    0,    -1 },
	{ ImGuiKey_DownArrow,   IMW_MOD_ANY,     "\033OB",    0,    +1 },
	{ ImGuiKey_RightArrow,  IMW_MOD_ANY,     "\033[C",    0,    -1 },
	{ ImGuiKey_RightArrow,  IMW_MOD_ANY,     "\033OC",    0,    +1 },
	{ ImGuiKey_LeftArrow,   IMW_MOD_ANY,     "\033[D",    0,    -1 },
	{ ImGuiKey_LeftArrow,   IMW_MOD_ANY,     "\033OD",    0,    +1 },

	{ ImGuiKey_Home,        IMW_MOD_ANY,     "\033[H",    0,    -1 },
	{ ImGuiKey_End,         IMW_MOD_ANY,     "\033[F",    0,    -1 },
	{ ImGuiKey_PageUp,      IMW_MOD_ANY,     "\033[5~",   0,     0 },
	{ ImGuiKey_PageDown,    IMW_MOD_ANY,     "\033[6~",   0,     0 },
	{ ImGuiKey_Insert,      IMW_MOD_ANY,     "\033[2~",   0,     0 },
	{ ImGuiKey_Delete,      IMW_MOD_ANY,     "\033[3~",   0,     0 },

	{ ImGuiKey_F1,          IMW_MOD_ANY,     "\033OP",    0,     0 },
	{ ImGuiKey_F2,          IMW_MOD_ANY,     "\033OQ",    0,     0 },
	{ ImGuiKey_F3,          IMW_MOD_ANY,     "\033OR",    0,     0 },
	{ ImGuiKey_F4,          IMW_MOD_ANY,     "\033OS",    0,     0 },
	{ ImGuiKey_F5,          IMW_MOD_ANY,     "\033[15~",  0,     0 },
	{ ImGuiKey_F6,          IMW_MOD_ANY,     "\033[17~",  0,     0 },
	{ ImGuiKey_F7,          IMW_MOD_ANY,     "\033[18~",  0,     0 },
	{ ImGuiKey_F8,          IMW_MOD_ANY,     "\033[19~",  0,     0 },
	{ ImGuiKey_F9,          IMW_MOD_ANY,     "\033[20~",  0,     0 },
	{ ImGuiKey_F10,         IMW_MOD_ANY,     "\033[21~",  0,     0 },
	{ ImGuiKey_F11,         IMW_MOD_ANY,     "\033[23~",  0,     0 },
	{ ImGuiKey_F12,         IMW_MOD_ANY,     "\033[24~",  0,     0 },
};
