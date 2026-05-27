/*	See LICENSE for license details.

	Defines class Terminal: an instance owns a private Emulator (PTY +
	parser state) and renders into the current ImGui window.
	
*/

#pragma once

#include <imgui.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>

extern "C"
{
#include "emulator.h"
}

class Terminal;

//  Adapter-side per-cell window state.
struct TermWin
{
	int mode;
	int cursor;
	int cw, ch;
	int tw, th;
	int w, h;
};

//  Per-variant font slot.
struct Font
{
	int ascent, descent, height, width;
	int badslant, badweight;
	float pxsize;
	ImFont *match;
};

//  Drawing context.
struct DC
{
	Font font, bfont, ifont, ibfont;
};

//  Parsed entry from rgb.txt.
struct ColorEntry
{
	uint8_t r, g, b;
	std::string canonical;
};

//  One packed glyph for rendering.
struct ImwGlyphSpec
{
	ImFont *font;
	Font *src;
	Rune codepoint;
	int x, y;
};

//  Per-row glyph-cache draw op: RECT, TEXT, PUSH_CLIP, POP_CLIP.
struct DrawOp
{
	enum Kind : uint8_t
	{
		RECT,
		TEXT,
		PUSH_CLIP,
		POP_CLIP
	} kind;
	ImVec2 p0, p1;
	ImU32 col;
	ImFont *font;
	uint8_t bytes[8];
	uint8_t len;
};

struct Shortcut
{
	ImGuiKeyChord chord;
	void (*func)(Terminal *, const Arg *);
	Arg arg;
};
struct MouseShortcut
{
	ImGuiKeyChord mods;
	ImGuiMouseButton button;
	void (*func)(Terminal *, const Arg *);
	Arg arg;
	unsigned int release;
};
struct Key
{
	ImGuiKey k;
	ImGuiKeyChord mods;
	const char *s;
	signed char appkey;
	signed char appcursor;
};

class Terminal
{
public:
	Terminal() = default;
	~Terminal();

	// User API
	void init(int cols, int rows, char **argv);
	void shutdown();
	void draw_canvas();
	void draw_widget(const char *id);
	void dump_json(std::FILE *out);
	void set_transparent(bool on);
	bool is_transparent() const;
	bool is_alive() const;

	void set_font_size(float px);
	float get_font_size();
	size_t ttyread(); /* tests use this to drain the PTY */

	/* Emulator callback receivers. Public because the cb_* extern "C"
		functions dispatch to these. */
	void on_bell();
	void on_clipcopy();
	void on_die(const char *msg);
	void on_drawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og);
	void on_drawline(Line line, int x1, int y, int x2);
	void on_finishdraw();
	void on_loadcols();
	int on_setcolorname(int idx, const char *name);
	int on_getcolor(int idx, unsigned char *r, unsigned char *g, unsigned char *b);
	void on_seticontitle(char *s);
	void on_settitle(char *s);
	int on_setcursor(int c);
	void on_setmode(int set, unsigned int flags);
	void on_setpointermotion(int x);
	void on_setsel(char *s);
	int on_startdraw();
	void on_ximspot(int x, int y);

private:
	static constexpr int IMW_COLORS_LEN = 260;

	// ---------- adapter state ----------
	Emulator e{};
	ImU32 colors[IMW_COLORS_LEN]{};
	std::vector<ColorEntry> rgb_db;
	TermWin tw{};
	char title[256] = "Terminal";
	bool selecting = false;
	int drag_sel_type = SEL_REGULAR;
	int last_motion_x = -1;
	int last_motion_y = -1;
	bool was_focused = false;
	bool transparent_bg = false;
	double blink_last_toggle_ms = 0.0;
	bool metrics_derived = false;

	// Per-frame render context
	ImVec2 canvas_pos{};
	ImDrawList *dl = nullptr;
	std::vector<std::vector<DrawOp>> row_ops;
	std::vector<DrawOp> overlay_ops;
	std::vector<DrawOp> *emit_target = nullptr;
	ImwGlyphSpec specs_buf[1024];

	// ---------- private helpers ----------
	void load_fonts_once();
	void load_rgb_db();
	bool parse_color(const char *name, uint8_t *r, uint8_t *g, uint8_t *b);
	bool lookup_rgb(const char *name, uint8_t *r, uint8_t *g, uint8_t *b);
	bool resolve_color_at(int i, const char *name, ImU32 *out);
	void clear_rect(int x1, int y1, int x2, int y2, ImU32 col);
	ImU32 resolve_glyph_color(uint32_t c);
	void drawglyph_internal(Glyph g, int x, int y);
	int makeglyphfontspecs(ImwGlyphSpec *specs, const Glyph *glyphs, int len, int x, int y);
	void drawglyphfontspecs(const ImwGlyphSpec *specs, Glyph base, int len, int x, int y);
	void emit_rect(ImVec2 p0, ImVec2 p1, ImU32 col);
	void emit_text(ImFont *font, ImVec2 pos, ImU32 col, const char *txt, int n);
	void emit_push_clip(ImVec2 p0, ImVec2 p1);
	void emit_pop_clip();
	void replay_ops(const std::vector<DrawOp> &ops, ImVec2 origin, ImDrawList *dl);
	void finalize_metrics();
	void handle_resize(ImVec2 avail);
	void pump_pty();
	void tick_blink();
	void dispatch_keyboard();
	void dispatch_mouse();
	void dispatch_mouse_select();
	void dispatch_mouse_report();
	void dispatch_mouse_mshortcuts();
	void emit_mouse(int btn_code, int col, int row, int mods, bool release);
	void emit_mouse_default(int btn_code, int col, int row, int mods, bool release);
	void emit_mouse_sgr(int btn_code, int col, int row, int mods, bool release);
	void pixel_to_cell(ImVec2 p, int *col, int *row);

	// JSON dump helpers
	const char *dump_font_name(ImFont *f);
	void dump_color(std::FILE *out, ImU32 c);
	void dump_jstr(std::FILE *out, const char *s, int len);
	void dump_op(std::FILE *out, const DrawOp &op);
	void dump_oplist(std::FILE *out, const std::vector<DrawOp> &ops);
	// ---- sh_* static wrappers (for shortcuts[] / mshortcuts[]) ----
	static void s_clipcopy(Terminal *, const Arg *);
	static void s_clippaste(Terminal *, const Arg *);
	static void s_selpaste(Terminal *, const Arg *);
	static void s_numlock(Terminal *, const Arg *);
	static void s_ttysend(Terminal *, const Arg *);
	static void s_printscreen(Terminal *, const Arg *);
	static void s_toggleprinter(Terminal *, const Arg *);
	// ---- static config ----
	static DC s_dc;
	static bool s_fonts_loaded;

#ifdef _WIN32
	static constexpr const char *font = "Consolas:pixelsize=16:antialias=true:autohint=true";
#else
	static constexpr const char *font = "Menlo:pixelsize=16:antialias=true:autohint=true";
#endif

	static constexpr float s_cwscale = 1.0f;
	static constexpr float s_chscale = 1.0f;
	static constexpr int s_borderpx = 2;
	static constexpr unsigned int s_cursorthickness = 2;
	static constexpr unsigned int s_blinktimeout = 800;
	static constexpr unsigned int s_defaultrcs = 257;
	static constexpr unsigned int s_defaultattr = 11;
	static constexpr int s_ignoremod = 0;
	static constexpr int s_forcemousemod = ImGuiMod_Shift;
	static bool imw_match_mods(ImGuiKeyChord wanted, int got);

	static constexpr ImGuiKeyChord IMW_MOD_ANY = (ImGuiKeyChord) -1;
	static constexpr ImGuiKeyChord TERMMOD = ImGuiMod_Ctrl | ImGuiMod_Shift;
	static constexpr int IMW_MB_WHEELUP = 5;
	static constexpr int IMW_MB_WHEELDOWN = 6;

	static constexpr const char *s_colorname_low[16] = {
	    "black",
	    "red3",
	    "green3",
	    "yellow3",
	    "blue2",
	    "magenta3",
	    "cyan3",
	    "gray90",
	    "gray50",
	    "red",
	    "green",
	    "yellow",
	    "#5c5cff",
	    "magenta",
	    "cyan",
	    "white",
	};
	static constexpr const char *s_colorname_high[4] = {
	    "#cccccc",
	    "#555555",
	    "gray90",
	    "black",
	};
	static constexpr MouseShortcut s_mshortcuts[5] = {
	    {Terminal::IMW_MOD_ANY, ImGuiMouseButton_Middle, Terminal::s_selpaste, {0}, 1},
	    {ImGuiMod_Shift, Terminal::IMW_MB_WHEELUP, Terminal::s_ttysend, {.s = "\033[5;2~"}, 0},
	    {Terminal::IMW_MOD_ANY, Terminal::IMW_MB_WHEELUP, Terminal::s_ttysend, {.s = "\031"},
		0},
	    {ImGuiMod_Shift, Terminal::IMW_MB_WHEELDOWN, Terminal::s_ttysend, {.s = "\033[6;2~"},
		0},
	    {Terminal::IMW_MOD_ANY, Terminal::IMW_MB_WHEELDOWN, Terminal::s_ttysend, {.s = "\005"},
		0},
	};
	static constexpr Shortcut s_shortcuts[5] = {
	    {ImGuiKey_C | Terminal::TERMMOD, Terminal::s_clipcopy, {0}},
	    {ImGuiKey_V | Terminal::TERMMOD, Terminal::s_clippaste, {0}},
	    {ImGuiKey_Y | Terminal::TERMMOD, Terminal::s_selpaste, {0}},
	    {ImGuiKey_Insert | ImGuiMod_Shift, Terminal::s_selpaste, {0}},
	    {ImGuiKey_NumLock | Terminal::TERMMOD, Terminal::s_numlock, {0}},
	};
	static constexpr int s_selmasks[3] = {0, 0, ImGuiMod_Alt};
	static constexpr Key s_key[32] = {
	    {ImGuiKey_Tab, ImGuiMod_Shift, "\033[Z", 0, 0},
	    {ImGuiKey_Tab, Terminal::IMW_MOD_ANY, "\t", 0, 0},
	    {ImGuiKey_Backspace, Terminal::IMW_MOD_ANY, "\177", 0, 0},
	    {ImGuiKey_Escape, Terminal::IMW_MOD_ANY, "\033", 0, 0},
	    {ImGuiKey_Enter, Terminal::IMW_MOD_ANY, "\r", 0, 0},
	    {ImGuiKey_UpArrow, Terminal::IMW_MOD_ANY, "\033[A", 0, -1},
	    {ImGuiKey_UpArrow, Terminal::IMW_MOD_ANY, "\033OA", 0, +1},
	    {ImGuiKey_DownArrow, Terminal::IMW_MOD_ANY, "\033[B", 0, -1},
	    {ImGuiKey_DownArrow, Terminal::IMW_MOD_ANY, "\033OB", 0, +1},
	    {ImGuiKey_RightArrow, Terminal::IMW_MOD_ANY, "\033[C", 0, -1},
	    {ImGuiKey_RightArrow, Terminal::IMW_MOD_ANY, "\033OC", 0, +1},
	    {ImGuiKey_LeftArrow, Terminal::IMW_MOD_ANY, "\033[D", 0, -1},
	    {ImGuiKey_LeftArrow, Terminal::IMW_MOD_ANY, "\033OD", 0, +1},
	    {ImGuiKey_Home, Terminal::IMW_MOD_ANY, "\033[H", 0, -1},
	    {ImGuiKey_End, Terminal::IMW_MOD_ANY, "\033[F", 0, -1},
	    {ImGuiKey_PageUp, Terminal::IMW_MOD_ANY, "\033[5~", 0, 0},
	    {ImGuiKey_PageDown, Terminal::IMW_MOD_ANY, "\033[6~", 0, 0},
	    {ImGuiKey_Insert, Terminal::IMW_MOD_ANY, "\033[2~", 0, 0},
	    {ImGuiKey_Delete, Terminal::IMW_MOD_ANY, "\033[3~", 0, 0},
	    {ImGuiKey_F1, Terminal::IMW_MOD_ANY, "\033OP", 0, 0},
	    {ImGuiKey_F2, Terminal::IMW_MOD_ANY, "\033OQ", 0, 0},
	    {ImGuiKey_F3, Terminal::IMW_MOD_ANY, "\033OR", 0, 0},
	    {ImGuiKey_F4, Terminal::IMW_MOD_ANY, "\033OS", 0, 0},
	    {ImGuiKey_F5, Terminal::IMW_MOD_ANY, "\033[15~", 0, 0},
	    {ImGuiKey_F6, Terminal::IMW_MOD_ANY, "\033[17~", 0, 0},
	    {ImGuiKey_F7, Terminal::IMW_MOD_ANY, "\033[18~", 0, 0},
	    {ImGuiKey_F8, Terminal::IMW_MOD_ANY, "\033[19~", 0, 0},
	    {ImGuiKey_F9, Terminal::IMW_MOD_ANY, "\033[20~", 0, 0},
	    {ImGuiKey_F10, Terminal::IMW_MOD_ANY, "\033[21~", 0, 0},
	    {ImGuiKey_F11, Terminal::IMW_MOD_ANY, "\033[23~", 0, 0},
	    {ImGuiKey_F12, Terminal::IMW_MOD_ANY, "\033[24~", 0, 0},
	};
};
