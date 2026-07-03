/* See LICENSE for license details. */
#pragma once

/* ============================================================
   terminal.h  -  ImGui-native terminal emulator.

   Usage:
       #include "terminal.h"      // declarations
       // link terminal.cpp
       Terminal t;
       t.init(80, 24, nullptr);   // spawn the shell
       t.draw_widget("###term");  // call once per ImGui frame
       t.shutdown();

   Requires ImGui built with IMGUI_ENABLE_FREETYPE + IMGUI_USE_WCHAR32
   ============================================================ */

/* ---- Emulator core: types & API ---- */
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <wchar.h>

#include <imgui.h>
#include <cstdio>
#include <string>
#include <vector>
#include <cstdint>

#ifdef _WIN32
#include <windows.h>
#ifdef _MSC_VER
typedef SSIZE_T ssize_t;
#endif
#endif

static inline int between(long x, long a, long b)
{
	return a <= x && x <= b;
}
static inline int is_truecol(uint32_t x)
{
	return (1 << 24) & x;
}
static inline int modbit(int x, int set, int bit)
{
	return set ? (x | bit) : (x & ~bit);
}

/* Sizing constants for struct definitions below. */
enum
{
	UTF_SIZ = 4,
	ESC_BUF_SIZ = 128 * UTF_SIZ,
	ESC_ARG_SIZ = 16,
	STR_BUF_SIZ = ESC_BUF_SIZ,
	STR_ARG_SIZ = ESC_ARG_SIZ,
};

enum glyph_attribute
{
	ATTR_NULL = 0,
	ATTR_BOLD = 1 << 0,
	ATTR_FAINT = 1 << 1,
	ATTR_ITALIC = 1 << 2,
	ATTR_UNDERLINE = 1 << 3,
	ATTR_BLINK = 1 << 4,
	ATTR_REVERSE = 1 << 5,
	ATTR_INVISIBLE = 1 << 6,
	ATTR_STRUCK = 1 << 7,
	ATTR_WRAP = 1 << 8,
	ATTR_WIDE = 1 << 9,
	ATTR_WDUMMY = 1 << 10,
	ATTR_BOLD_FAINT = ATTR_BOLD | ATTR_FAINT,
};

enum selection_mode
{
	SEL_IDLE = 0,
	SEL_EMPTY = 1,
	SEL_READY = 2
};

enum selection_type
{
	SEL_REGULAR = 1,
	SEL_RECTANGULAR = 2
};

enum selection_snap
{
	SNAP_WORD = 1,
	SNAP_LINE = 2
};

enum win_mode
{
	MODE_VISIBLE = 1 << 0,
	MODE_FOCUSED = 1 << 1,
	MODE_APPKEYPAD = 1 << 2,
	MODE_MOUSEBTN = 1 << 3,
	MODE_MOUSEMOTION = 1 << 4,
	MODE_REVERSE = 1 << 5,
	MODE_KBDLOCK = 1 << 6,
	MODE_HIDE = 1 << 7,
	MODE_APPCURSOR = 1 << 8,
	MODE_MOUSESGR = 1 << 9,
	MODE_8BIT = 1 << 10,
	MODE_BLINK = 1 << 11,
	MODE_FBLINK = 1 << 12,
	MODE_FOCUS = 1 << 13,
	MODE_MOUSEX10 = 1 << 14,
	MODE_MOUSEMANY = 1 << 15,
	MODE_BRCKTPASTE = 1 << 16,
	MODE_NUMLOCK = 1 << 17,
	MODE_MOUSE = MODE_MOUSEBTN | MODE_MOUSEMOTION | MODE_MOUSEX10 | MODE_MOUSEMANY,
};

typedef unsigned char uchar;
typedef unsigned int uint;
typedef unsigned long ulong;
typedef unsigned short ushort;

typedef uint_least32_t Rune;

typedef struct
{
	Rune u;	     /* character code */
	ushort mode; /* attribute flags */
	uint32_t fg; /* foreground  */
	uint32_t bg; /* background  */
} Glyph;

static inline int attrcmp(Glyph a, Glyph b)
{
	return a.mode != b.mode || a.fg != b.fg || a.bg != b.bg;
}

typedef Glyph *Line;

typedef union
{
	int i;
	uint ui;
	float f;
	const void *v;
	const char *s;
} Arg;

typedef struct
{
	Glyph attr; /* current char attributes */
	int x;
	int y;
	char state;
} TCursor;

typedef struct
{
	int mode;
	int type;
	int snap;
	/*
 * Selection variables:
 * nb: normalized coordinates of the beginning of the selection
 * ne: normalized coordinates of the end of the selection
 * ob: original coordinates of the beginning of the selection
 * oe: original coordinates of the end of the selection
 */
	struct
	{
		int x, y;
	} nb, ne, ob, oe;

	int alt;
} Selection;

/* Internal representation of the screen */
typedef struct
{
	int row;	 /* nb row */
	int col;	 /* nb col */
	Line *line;	 /* screen */
	Line *alt;	 /* alternate screen */
	int *dirty;	 /* dirtyness of lines */
	TCursor c;	 /* cursor */
	int ocx;	 /* old cursor col */
	int ocy;	 /* old cursor row */
	int top;	 /* top scroll limit */
	int bot;	 /* bottom scroll limit */
	int mode;	 /* terminal mode flags */
	int esc;	 /* escape state flags */
	char trantbl[4]; /* charset table translation */
	int charset;	 /* current charset */
	int icharset;	 /* selected charset for sequence */
	int *tabs;
	Rune lastc; /* last printed char outside of sequence, 0 if control */
} Term;

/* CSI Escape sequence structs */
/* ESC '[' [[ [<priv>] <arg> [;]] <mode> [<mode>]] */
typedef struct
{
	char buf[ESC_BUF_SIZ]; /* raw string */
	size_t len;	       /* raw string length */
	char priv;
	int arg[ESC_ARG_SIZ];
	int narg; /* nb of args */
	char mode[2];
} CSIEscape;

/* STR Escape sequence structs */
/* ESC type [[ [<priv>] <arg> [;]] <mode>] ESC '\' */
typedef struct
{
	char type;  /* ESC type ... */
	char *buf;  /* allocated raw string */
	size_t siz; /* allocation size */
	size_t len; /* raw string length */
	char *args[STR_ARG_SIZ];
	int narg; /* nb of args */
} STREscape;

#ifndef BUFSIZ
#include <stdio.h>
#endif

/* Stateless helpers, no Emulator arg. */
void *xmalloc(size_t);
void *xrealloc(void *, size_t);
char *xstrdup(const char *);
size_t utf8encode(Rune, char *);
void die(const char *, ...);

/* config globals (defined in terminal.cpp) */
extern char *utmp;
extern char *scroll;
extern char *stty_args;
extern char *vtiden;
extern wchar_t *worddelimiters;
extern int allowaltscreen;
extern int allowwindowops;
extern char *termname;
extern unsigned int tabspaces;
extern unsigned int defaultfg;
extern unsigned int defaultbg;
extern unsigned int defaultcs;

/* ---- ImGui adapter ----
   class Terminal owns the emulator state (PTY + parser) and renders
   into the current ImGui window. ---- */

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
	bool draw_canvas();
	bool draw_widget(const char *id);
	void set_retained(bool on);
	void dump_json(std::FILE *out);
	void set_transparent(bool on);
	bool is_transparent() const;
	bool is_alive() const;
	void tick();

	int draw_count() const { return m_draw_count; }
	void set_font_size(float px);
	float get_font_size();
	size_t ttyread(); /* tests use this to drain the PTY */

private:
	// Terminal output ops, driven by the emulator core.
	void copy_selection();
	void handle_child_exit(const char *msg);
	void draw_cursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og);
	void draw_line(Line line, int x1, int y, int x2);
	void load_colors();
	int set_color_name(int idx, const char *name);
	int get_color(int idx, unsigned char *r, unsigned char *g, unsigned char *b);
	void set_title(char *s);
	int set_cursor_style(int c);
	void set_win_mode(int set, unsigned int flags);
	void set_clipboard(char *s);
	int begin_draw();

	static constexpr int IMW_COLORS_LEN = 260;

	// ---------- adapter state ----------
	// ---------- emulator core state (was struct Emulator) ----------
	// All value-initialized: a stack-allocated Terminal must start zeroed
	// (str.buf == NULL, read_buflen == 0, …) before init() runs.
	Term term{};
	Selection sel{};
	CSIEscape csi{};
	STREscape str{};
#ifndef _WIN32
	int iofd{};
	int cmdfd{};
	pid_t pid{};
#else
	int iofd{};
	int cmdfd{}; /* opaque on Windows; >= 0 marks the tty as open */
	HPCON w32_hpc{};
	HANDLE w32_pipe_in{};
	HANDLE w32_pipe_out{};
	HANDLE w32_proc{};
	HANDLE w32_reader_ready{};
	int w32_reader_signaled{};
#endif
	char read_buf[BUFSIZ]{};
	int read_buflen{};
	TCursor saved_cursor[2]{};
	int alive{};

	// ---------- emulator core methods (was free functions) ----------
	void selinit();
	int tlinelen(int y);
	void selstart(int col, int row, int snap);
	void selextend(int col, int row, int type, int done);
	void selnormalize();
	int selected(int x, int y);
	void selsnap(int *x, int *y, int direction);
	char *getsel();
	void selclear();
	void emu_die(const char *fmt, ...);
	int ttynew(const char *line, char *cmd, const char *out, char **args);
	void ttywrite(const char *s, size_t n, int may_echo);
	void ttywriteraw(const char *s, size_t n);
	void ttyresize(int tw, int th);
	void ttyhangup();
	int tattrset(int attr);
	void tsetdirt(int top, int bot);
	void tsetdirtattr(int attr);
	void tfulldirt();
	void tcursor(int mode);
	void treset();
	void tnew(int col, int row);
	void tswapscreen();
	void tscrolldown(int orig, int n);
	void tscrollup(int orig, int n);
	void selscroll(int orig, int n);
	void tnewline(int first_col);
	void csiparse();
	void tmoveato(int x, int y);
	void tmoveto(int x, int y);
	void tsetchar(Rune u, const Glyph *attr, int x, int y);
	void tclearregion(int x1, int y1, int x2, int y2);
	void tdeletechar(int n);
	void tinsertblank(int n);
	void tinsertblankline(int n);
	void tdeleteline(int n);
	int32_t tdefcolor(const int *attr, int *npar, int l);
	void tsetattr(const int *attr, int l);
	void tsetscroll(int t, int b);
	void tsetmode(int priv, int set, const int *args, int narg);
	void csihandle();
	void csidump();
	void csireset();
	void osc_color_response(int num, int index, int is_osc4);
	void strhandle();
	void strparse();
	void strdump();
	void strreset();
	void tprinter(const char *s, size_t len);
	void tdumpsel();
	void tdumpline(int n);
	void tdump();
	void tputtab(int n);
	void tdefutf8(char ascii);
	void tdeftran(char ascii);
	void tdectest(char c);
	void tstrsequence(uchar c);
	void tcontrolcode(uchar ascii);
	int eschandle(uchar ascii);
	void tputc(Rune u);
	int twrite(const char *buf, int buflen, int show_ctrl);
	void tresize(int col, int row);
	void resettitle();
	void drawregion(int x1, int y1, int x2, int y2);
	void draw();
	void redraw();
#ifdef _WIN32
	int w32_build_cmdline(char *buf, size_t cap, char *cmd, char **args);
	int w32_spawn_pcon_child(LPWSTR cmdline, HPCON hpc, HANDLE *out_proc);
	void w32_ttynew_cleanup();
	static unsigned __stdcall w32_monitor_thread(void *arg);
#endif

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
	double cursor_blink_timer = 0.0;
	bool cursor_blink_on = true;
	bool cursor_blinking = false;
	bool metrics_derived = false;
	bool m_changed = true; /* force first frame */
	bool m_retained = false;
	int m_draw_count = 0;

	// Per-frame render context
	ImVec2 canvas_pos{};
	ImDrawList *dl = nullptr;
	std::vector<std::vector<DrawOp>> row_ops;
	std::vector<DrawOp> overlay_ops;
	std::vector<DrawOp> *emit_target = nullptr;
	ImwGlyphSpec specs_buf[1024];

	// ---------- private helpers ----------
	void load_fonts(float px_override);
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
	static constexpr unsigned int s_cursorblinktimeout = 750;
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
	static constexpr Key s_key[] = {
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
