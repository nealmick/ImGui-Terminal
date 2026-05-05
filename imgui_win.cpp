/*
	imgui_win.cpp — ImGui-native windowing layer for Suckless terminal.
	Implements the contract declared in win.h.

	RULES :
	 - No GLFW. No OpenGL, No Vulkan. No platform-window-system code.
	 - Only ImGui APIs for input/clipboard/cursor/render.
	 - The host owns the renderer, window, and main event loop.
	 - This file exposes only: term_init / term_draw_widget / term_shutdown
	   and the 16 win.h functions.

	Naming convention:
	 - x<name>    — win.h contract surface (linker-visible, called by
	                st.c). Mirrors upstream's "x" prefix for "X-window
	                adapter." Lives in the extern "C" thunk block;
	                each is a one-line forward to imw_<name>.
	 - imw_<name> — adapter-internal implementation. "imw" = imgui_win.
	                The real bodies live here.
	The split lets implementations have descriptive names while keeping
	the linker contract st.c expects. Compiler inlines the thunks at
	any optimization level.
*/

#include "imgui.h"
#include "imgui_freetype.h"

#include <fontconfig/fontconfig.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/select.h>
#endif

/* Executable-path lookup for resolving rgb.txt relative to the binary
 * (instead of the CWD). No portable C API for this — short platform block. */
#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include <algorithm>
#include <string>
#include <vector>

extern "C" {
#include "st.h"
#include "win.h"
#ifdef _WIN32
/* ConPTY read-pipe handle, defined in st.c */
extern HANDLE w32_pipe_in;
#endif
}


/*  Sentinel: matches any modifier state. Mirrors X11's XK_ANY_MOD.  */
#define IMW_MOD_ANY ((ImGuiKeyChord)-1)

/*  Conventional st modifier prefix — Ctrl+Shift, mirrors config.def.h:155.  */
#define TERMMOD     (ImGuiMod_Ctrl | ImGuiMod_Shift)

/*
	Keyboard shortcut: chord triggers a function. Mirrors x.c's `Shortcut`
	with ImGuiKeyChord (mods | ImGuiKey) repacacing (uint mod, KeySym keysym).
*/
typedef struct {
	ImGuiKeyChord chord;
	void        (*func)(const Arg *);
	Arg           arg;
} Shortcut;

/*  Mouse shortcut: button + mods triggers a function on press or release.  */
typedef struct {
	ImGuiKeyChord     mods;
	ImGuiMouseButton  button;
	void            (*func)(const Arg *);
	Arg               arg;
	unsigned int      release;
} MouseShortcut;

/*
	Synthetic mouse-button constants for wheel events. ImGui exposes the
	wheel as a separate axis (io.MouseWheel), not a button — but the
	mshortcuts[] table walks by `button` field, so we encode wheel
	direction as out-of-range button values for table-walk uniformity.
	Mirrors X11's Button4/Button5 convention used by config.def.h.

	ImGuiMouseButton_COUNT == 5 (Left, Right, Middle, X1, X2). Pick 5/6
	since they sit immediately past the real buttons and config-imgui.def.h
	needs to reference them in mshortcuts[] entries. The `button` field
	in MouseShortcut is `int`-typed (ImGuiMouseButton is a typedef int),
	so storing 5/6 is well-defined.
*/
#define IMW_MB_WHEELUP    5
#define IMW_MB_WHEELDOWN  6

/*
	Special-key dispatch entry: ImGuiKey + mods -> bytes. appkey/appcursor
	are three-valued (-1/0/+1/+2) per x.c::kmap.
*/
typedef struct {
	ImGuiKey      k;
	ImGuiKeyChord mods;
	const char   *s;
	signed char   appkey;
	signed char   appcursor;
} Key;

/*
	Forward declarations for adapter-side shortcut handlers. Bodies are
	further down in this file. The st.c-side handlers (sendbreak,
	printscreen, printsel, toggleprinter) are already declared in st.h.

	zoom/zoomabs/zoomreset are stubs reserved for font-reload-on-zoom.
	They're not currently referenced by the shortcuts[] array, but the
	prototypes stay so the bodies below have something to declare —
	IMW_UNUSED keeps -Wall quiet until they're wired in. MSVC has no
	equivalent attribute in this position, so the macro is empty there.
*/
#if defined(__GNUC__) || defined(__clang__)
#define IMW_UNUSED __attribute__((unused))
#else
#define IMW_UNUSED
#endif
static void clipcopy(const Arg *);
static void clippaste(const Arg *);
static void selpaste(const Arg *);
static void numlock(const Arg *);
static void zoom(const Arg *)      IMW_UNUSED;
static void zoomabs(const Arg *)   IMW_UNUSED;
static void zoomreset(const Arg *) IMW_UNUSED;
static void ttysend(const Arg *)   IMW_UNUSED;

/*
	Adapter config — provides storage for st.h's externs (utmp, tabspaces,
	worddelimiters, etc.), adapter-only statics (borderpx, defaultattr,
	colorname_low/high), and the input arrays (key[], shortcuts[],
	mshortcuts[], selmasks[]). Generated from config-imgui.def.h on first
	build (gitignored, user-editable) — same convention as upstream's
	config.def.h -> config.h.

	SINGLE-TU RULE: this file and only this file includes config-imgui.h.
	See the warning at the top of config-imgui.def.h.
*/
#include "config-imgui.h"

/*  Local state  */

struct TermWin {
	int  mode;     /*  bitmask of MODE_*                        */
	int  cursor;   /*  0..7 — cursor shape                      */
	int  cw, ch;   /*  cell width/height in pixels (logical)    */
	int  tw, th;   /*  terminal raster size = cols*cw, rows*ch  */
	int  w,  h;    /*  widget canvas size in pixels             */
};

static TermWin tw;

/*  Per-variant font slot — mirrors x.c's Font struct  */
struct Font {
	int      ascent;
	int      descent;
	int      height;     /*  line height = ascent + |descent|         */
	int      width;      /*  advance of representative glyph          */
	int      badslant;   /*  1 if requested italic but got roman      */
	int      badweight;  /*  1 if requested bold but got medium       */
	float    pxsize;     /*  size we asked AddFontFromFileTTF for     */
	ImFont  *match;      /*  the loaded ImGui font                    */
};

/*  Drawing context — mirrors x.c's dc.  */
struct DC {
	Font font, bfont, ifont, ibfont;
	bool metrics_derived;  /*  set after first frame's atlas build  */
};

static DC dc;

/*
	PTY master fd — captured from ttynew() at startup, polled per-frame.
	st.c stashes its own copy internally (cmdfd); ours is just for poll().
*/
static int imw_cmdfd = -1;

/*
	Mouse selection-drag state. Set when left-click lands on our canvas;
	cleared on left-release. While set, IsMouseDragging extends the
	selection even if the mouse leaves the canvas (intentional — user
	can drag past the visible area to extend). Mirrors the implicit state
	x.c carries via sel.mode SEL_EMPTY/SEL_READY transitions.
*/
static bool imw_selecting = false;

/*
	Mouse-report dedupe state. Mirrors x.c::mousereport's static
	ox/oy at line 372: dedupe motion events by (x, y) so MODE_MOUSEMANY
	doesn't flood the PTY at 60Hz when the cursor hovers a single cell.
	Updated after EVERY successful emit (press, release, wheel, motion)
	so a motion arriving at the same cell as the most-recent non-motion
	event also suppresses (x.c lines 403-404).

	Reset on MODE_MOUSE flag flip via the xsetmode hook so a mid-drag
	mode change starts fresh. Initial -1/-1 ensures the first motion
	after startup always emits.
*/
static int imw_last_motion_x = -1;
static int imw_last_motion_y = -1;

/*
	Focus-event-reporting edge tracker. When MODE_FOCUS is set
	(DECSET 1004 — app requested focus events), we emit \033[I on
	focus gain and \033[O on focus loss. Edge-triggered against this
	flag so the sequence fires once per transition, not every frame.

	Initial false means: if the first frame already has focus, we
	emit a spurious \033[I at startup. tmux/vim handle that gracefully
	(treat as a regular focus-in), so it's not worth gating against.
*/
static bool imw_was_focused = false;

/*
	Title — set by xsettitle, displayed via ImGui::Begin label using the
	###id pattern.
*/
static char term_title[256] = "Terminal";

/*
	Indexed palette. 256 standard + 4 extended (cs/rcs/fg/bg) = 260.
	Mirrors x.c's `dc.col` allocated to MAX(LEN(colorname), 256).
*/
#define IMW_COLORS_LEN 260
static ImU32 colors[IMW_COLORS_LEN];

/*
	X11 rgb.txt parsed at startup. One entry per line of rgb.txt;
	the canonical form is lowercase + whitespace stripped, used for lookup.
*/
struct ColorEntry {
	uint8_t  r, g, b;
	std::string canonical;
};
static std::vector<ColorEntry> imw_rgb_db;

/*
	One packed glyph for rendering. Output of xmakeglyphfontspecs, input to
	xdrawglyphfontspecs. Coords are widget-relative; canvas_pos is added at
	ImDrawList emit time

	`font` is the ImFont* used to render THIS glyph (might be a fallback
	for rune-not-in-primary cases when FRC is implemented).
	`src` points at whichever dc.{font,bfont,ifont,ibfont} was the PRIMARY
	variant for this run's attributes. Stamped here once by the spec packer
	so xdrawglyphfontspecs can read src->badweight / src->badslant directly
	without re-deriving from base.mode.
*/
struct ImwGlyphSpec {
	ImFont *font;
	Font   *src;
	Rune    codepoint;
	int     x, y;
};

/*
	Per-frame render context — set at the top of term_draw_widget, read by
	the rendering helpers (xclear, xdrawglyphfontspecs, etc.). Two fields,
	narrow on purpose so the helpers don't accumulate hidden state.
*/
struct ImwRenderCtx {
	ImVec2       canvas_pos;
	ImDrawList  *dl;
};
static ImwRenderCtx imw_ctx;

/*  Forward declarations — helpers defined further down.  */
static bool    imw_parse_color(const char *name, uint8_t *r, uint8_t *g, uint8_t *b);
static void    imw_load_rgb_db(void);
static uint8_t imw_sixd_to_8bit(int x);
static bool    imw_resolve_color_at(int i, const char *name, ImU32 *out);
static void    imw_clear(int x1, int y1, int x2, int y2, ImU32 col);
static inline ImU32 imw_resolve_glyph_color(uint32_t c);
static void    imw_drawglyphfontspecs(const ImwGlyphSpec *specs, Glyph base,
                                       int len, int x, int y);
static int     imw_makeglyphfontspecs(ImwGlyphSpec *specs, const Glyph *glyphs,
                                       int len, int x, int y);
static void    imw_drawglyph(Glyph g, int x, int y);

/*
	Pre-allocated row-spec buffer — sized for max realistic terminal width.
	Mirrors x.c's xw.specbuf (sized in xresize to term.col). One spec per
	non-WDUMMY glyph; never grows.
*/
#define IMW_SPECS_BUFLEN 1024
static ImwGlyphSpec imw_specs_buf[IMW_SPECS_BUFLEN];

/*
	Per-row glyph cache — record/replay infrastructure.

	Background: ImGui rebuilds the draw list every frame, while st.c's
	dirty-tracking only re-emits xdrawline calls for rows whose content
	actually changed. Without caching, we'd have to call redraw()
	(tfulldirt + draw) each frame to keep the visible canvas populated,
	which re-records every glyph at 60Hz even when nothing changed.

	With caching: xdrawline writes DrawOp entries into imw_row_ops[y]
	(replacing any previous content for that row). The replay loop in
	term_draw_canvas runs every frame, walking each row's cached ops
	and feeding them to the real ImDrawList. Cache invalidation rides
	on st.c's dirty array: when a row gets re-dirty, the next draw()
	calls xdrawline which clears the cache for that row and re-records.

	Cache correctness depends on every visual change marking rows dirty.
	Most paths land naturally:
	  - Glyph mutation (twrite, tputc, tclearregion, tscrollup/down,
	    selection extend/clear) all dirty inside st.c.
	  - Palette mutation (OSC 4/10/11/104/110-112): st.c calls
	    tfulldirt() inside the OSC handler (st.c:1945, 1970, 1983)
	    after xsetcolorname succeeds — adapter has nothing to do.
	  - Whole-screen MODE_REVERSE flip: imw_setmode calls redraw()
	    on the bit transition (mirrors x.c:1739).
	The two paths st.c can't see and the adapter must handle:
	  - MODE_BLINK toggle: imw_tick_blink calls tsetdirtattr(ATTR_BLINK)
	    after flipping MODE_BLINK so rows holding blinking content
	    re-emit with the new visibility (mirrors x.c:2017).
	  - Resize: imw_handle_resize calls redraw() because tresize only
	    auto-dirties on grow, not shrink (st.c:2641-2647). Width-only
	    shrink would otherwise leave stale ops with old x-coords.

	The cursor is special: it's drawn on top of the row content every
	frame and may move independently of dirty rows, so xdrawcursor
	pushes to imw_overlay_ops (cleared at the start of each draw()
	cycle via xstartdraw — the overlay is per-frame, never cached).
*/
struct DrawOp {
	enum Kind : uint8_t { RECT, TEXT, PUSH_CLIP, POP_CLIP } kind;
	ImVec2  p0, p1;        /*  RECT: corners; TEXT: pos in p0; PUSH_CLIP: corners  */
	ImU32   col;           /*  RECT, TEXT  */
	ImFont *font;          /*  TEXT  */
	uint8_t bytes[8];      /*  TEXT — UTF-8 encoded codepoint  */
	uint8_t len;           /*  TEXT — byte count in `bytes`  */
};

static std::vector<std::vector<DrawOp>> imw_row_ops;   /*  per-row, sized to term.row  */
static std::vector<DrawOp>              imw_overlay_ops;
static std::vector<DrawOp>             *imw_emit_target = NULL;

static inline void
imw_emit_rect(ImVec2 p0, ImVec2 p1, ImU32 col)
{
	DrawOp op;
	op.kind = DrawOp::RECT;
	op.p0 = p0; op.p1 = p1; op.col = col;
	op.font = NULL; op.len = 0;
	imw_emit_target->push_back(op);
}

static inline void
imw_emit_text(ImFont *font, ImVec2 pos, ImU32 col, const char *txt, int n)
{
	DrawOp op;
	op.kind = DrawOp::TEXT;
	op.p0 = pos; op.p1 = ImVec2(0, 0); op.col = col;
	op.font = font;
	op.len = (uint8_t)((n > (int)sizeof op.bytes) ? sizeof op.bytes : n);
	memcpy(op.bytes, txt, op.len);
	imw_emit_target->push_back(op);
}

static inline void
imw_emit_push_clip(ImVec2 p0, ImVec2 p1)
{
	DrawOp op;
	op.kind = DrawOp::PUSH_CLIP;
	op.p0 = p0; op.p1 = p1; op.col = 0;
	op.font = NULL; op.len = 0;
	imw_emit_target->push_back(op);
}

static inline void
imw_emit_pop_clip(void)
{
	DrawOp op;
	op.kind = DrawOp::POP_CLIP;
	op.p0 = ImVec2(0, 0); op.p1 = ImVec2(0, 0); op.col = 0;
	op.font = NULL; op.len = 0;
	imw_emit_target->push_back(op);
}

/*
	Replay a recorded op-list to the real ImDrawList. Cache stores
	widget-relative coords; canvas_pos is added here at replay time so
	a window drag (which changes canvas_pos but doesn't dirty any rows)
	doesn't invalidate the cache. Drawing order is preserved (vector
	iteration). PushClipRect/PopClipRect are paired within
	imw_drawglyphfontspecs so the clip stack stays balanced across
	replays.
*/
static void
imw_replay_ops(const std::vector<DrawOp> &ops, ImVec2 origin, ImDrawList *dl)
{
	for (size_t i = 0; i < ops.size(); i++) {
		const DrawOp &op = ops[i];
		ImVec2 p0(origin.x + op.p0.x, origin.y + op.p0.y);
		ImVec2 p1(origin.x + op.p1.x, origin.y + op.p1.y);
		switch (op.kind) {
		case DrawOp::RECT:
			dl->AddRectFilled(p0, p1, op.col);
			break;
		case DrawOp::TEXT:
			dl->AddText(op.font, dc.font.pxsize, p0, op.col,
			            (const char *)op.bytes,
			            (const char *)op.bytes + op.len);
			break;
		case DrawOp::PUSH_CLIP:
			dl->PushClipRect(p0, p1, true);
			break;
		case DrawOp::POP_CLIP:
			dl->PopClipRect();
			break;
		}
	}
}

/*  Adapter implementations of the 16 win.h contract functions.  */

static void
imw_bell(void)
{
	/*  Bell intentionally not implemented.  */
}

static void
imw_clipcopy(void)
{
	/*  Push current selection to OS clipboard via ImGui's API.  */
	char *sel = getsel();
	if (sel) {
		ImGui::SetClipboardText(sel);
		free(sel);
	}
}

static void
imw_drawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	/*
		Cursor draws into the per-frame overlay (cleared at start of each
		draw cycle in imw_startdraw). Never goes into the row cache —
		cursor moves and blinks independently of dirty rows.

		Note: x.c::xdrawcursor (1528-1531) erases the previous cursor cell
		by redrawing the underlying glyph at (ox, oy). We don't, and don't
		need to: x.c renders into a persistent pixmap so stale cursor
		pixels survive between frames; our overlay is cleared every frame
		at imw_startdraw, and the row cache for oy already holds the
		underlying glyph (selection inversion included — xdrawline applies
		the XOR per-cell). Replay paints it before the overlay layers the
		new cursor on top. The erase x.c does is purely redundant in our
		model. og is therefore unused.
	*/
	(void)ox; (void)oy; (void)og;
	imw_emit_target = &imw_overlay_ops;

	/*  MODE_HIDE: nothing to draw. Mirrors x.c:1533.  */
	if (tw.mode & MODE_HIDE)
		return;

	/*
		Prune attributes that don't make sense on a cursor (BLINK, INVISIBLE,
		FAINT, REVERSE). Keep only the visual attrs that should pass through.
		Mirrors x.c:1539.
	*/
	g.mode &= ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_STRUCK | ATTR_WIDE;

	/*
		Color picking — MODE_REVERSE × selection × cursor-vs-rcursor.
		drawcol is for bar/underline shapes (block uses g.bg via xdrawglyph).
		Mirrors x.c:1541-1560 verbatim.
	*/
	ImU32 drawcol;
	if (tw.mode & MODE_REVERSE) {
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (selected(cx, cy)) {
			drawcol = colors[defaultcs];
			g.fg   = defaultrcs;
		} else {
			drawcol = colors[defaultrcs];
			g.fg   = defaultcs;
		}
	} else {
		if (selected(cx, cy)) {
			g.fg = defaultfg;
			g.bg = defaultrcs;
		} else {
			g.fg = defaultbg;
			g.bg = defaultcs;
		}
		drawcol = colors[g.bg];
	}

	/*  Render the new cursor. Mirrors x.c:1562-1606.  */
	int winx = borderpx + cx * tw.cw;
	int winy = borderpx + cy * tw.ch;

	if (tw.mode & MODE_FOCUSED) {
		switch (tw.cursor) {
		case 7:               /*  st extension: snowman ☃  */
			g.u = 0x2603;
			/*  fallthrough  */
		case 0:               /*  Blinking Block  */
		case 1:               /*  Blinking Block (default)  */
		case 2:               /*  Steady Block  */
			imw_drawglyph(g, cx, cy);
			break;
		case 3:               /*  Blinking Underline  */
		case 4:               /*  Steady Underline  */
			imw_clear(winx,
			           winy + tw.ch - (int)cursorthickness,
			           winx + tw.cw,
			           winy + tw.ch,
			           drawcol);
			break;
		case 5:               /*  Blinking Bar  */
		case 6:               /*  Steady Bar  */
			imw_clear(winx, winy,
			           winx + (int)cursorthickness,
			           winy + tw.ch,
			           drawcol);
			break;
		}
	} else {
		/*
			Unfocused: hollow rectangle outline (4 thin rects).
			Mirrors x.c:1589-1605. The cw-1/ch-1 offsets prevent the
			corner pixels from being painted twice.
		*/
		imw_clear(winx, winy,
		           winx + tw.cw - 1, winy + 1, drawcol);            /*  top     */
		imw_clear(winx, winy,
		           winx + 1, winy + tw.ch - 1, drawcol);            /*  left    */
		imw_clear(winx + tw.cw - 1, winy,
		           winx + tw.cw, winy + tw.ch - 1, drawcol);        /*  right   */
		imw_clear(winx, winy + tw.ch - 1,
		           winx + tw.cw, winy + tw.ch, drawcol);            /*  bottom  */
	}
}

static void
imw_drawline(Line line, int x1, int y, int x2)
{
	/*
		Mirrors x.c::xdrawline (line 1659). Two stages:
		  1. Pack the row's glyphs into specs via xmakeglyphfontspecs.
		  2. Walk the row, batching consecutive cells with matching
		     (mode, fg, bg) into runs; each run goes to xdrawglyphfontspecs.

		Selection inversion happens here per-cell (XOR ATTR_REVERSE on a
		COPY of the glyph's mode used as `base` for the run; we never
		mutate term.line).

		Cache: this is the only entry point that writes to imw_row_ops[y].
		Clear the row's existing ops first so old content from a previous
		draw of this row doesn't accumulate. After this function returns,
		the row's cache holds a complete, fresh op-list for replay.
	*/

	if (y >= 0 && y < (int)imw_row_ops.size())
		imw_row_ops[y].clear();
	imw_emit_target = (y >= 0 && y < (int)imw_row_ops.size())
	                  ? &imw_row_ops[y] : NULL;
	if (imw_emit_target == NULL)
		return;  /*  out-of-range row; shouldn't happen  */

	int numspecs = imw_makeglyphfontspecs(imw_specs_buf, &line[x1],
	                                       x2 - x1, x1, y);

	ImwGlyphSpec *specs = imw_specs_buf;
	Glyph base = {};
	Glyph neu;        /*  `new` is a C++ keyword  */
	int   i = 0;
	int   ox = 0;

	for (int x = x1; x < x2 && i < numspecs; x++) {
		neu = line[x];
		if (neu.mode == ATTR_WDUMMY)
			continue;
		if (selected(x, y))
			neu.mode ^= ATTR_REVERSE;
		if (i > 0 && ATTRCMP(base, neu)) {
			imw_drawglyphfontspecs(specs, base, i, ox, y);
			specs    += i;
			numspecs -= i;
			i = 0;
		}
		if (i == 0) {
			ox = x;
			base = neu;
		}
		i++;
	}
	if (i > 0)
		imw_drawglyphfontspecs(specs, base, i, ox, y);
}

static void
imw_finishdraw(void)
{
	/*
		No-op: under widget model, host owns the render flush.
		After term_draw_widget returns, the host calls ImGui::Render
		and its renderer backend submits the draw list. We never
		participate in flushing.
	*/
}

static void
imw_loadcols(void)
{
	imw_load_rgb_db();
	for (int i = 0; i < IMW_COLORS_LEN; i++) {
		ImU32 c;
		/*
			NULL name -> derive from index (mirrors x.c xloadcolor's
			"no name" path).
		*/
		if (!imw_resolve_color_at(i, NULL, &c))
			die("imgui_win: cannot derive color slot %d\n", i);
		colors[i] = c;
	}

	/*  One-time palette diagnostic — commented out for clean console.
	    Uncomment to verify parser + rgb.txt lookup at startup.

	auto dump = [](int i, const char *label) {
		ImU32 c = colors[i];
		fprintf(stderr, "  [%3d] %-12s = #%02x%02x%02x\n", i, label,
		    (c >> IM_COL32_R_SHIFT) & 0xff,
		    (c >> IM_COL32_G_SHIFT) & 0xff,
		    (c >> IM_COL32_B_SHIFT) & 0xff);
	};
	fprintf(stderr, "imgui_win: palette loaded (%zu rgb.txt entries)\n",
	    imw_rgb_db.size());
	dump(0,   "black");
	dump(1,   "red3");
	dump(9,   "red");
	dump(15,  "white");
	dump(16,  "cube[0,0,0]");
	dump(196, "cube[5,0,0]");
	dump(232, "gray232");
	dump(255, "gray255");
	dump(258, "fg");
	dump(259, "bg");
	*/
}

static int
imw_setcolorname(int x, const char *name)
{
	/*
		x.c::xsetcolorname accepts name==NULL to mean "reset to default" —
		st.c calls it that way during OSC color reset. We share the same
		inner resolver as xloadcols, which handles NULL by re-deriving
		from index just like xloadcolor does in x.c.
	*/
	if (!BETWEEN(x, 0, IMW_COLORS_LEN - 1))
		return 1;
	ImU32 c;
	if (!imw_resolve_color_at(x, name, &c))
		return 1;
	colors[x] = c;
	return 0;
}

static int
imw_getcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!BETWEEN(x, 0, IMW_COLORS_LEN - 1))
		return 1;
	ImU32 c = colors[x];
	*r = (c >> IM_COL32_R_SHIFT) & 0xff;
	*g = (c >> IM_COL32_G_SHIFT) & 0xff;
	*b = (c >> IM_COL32_B_SHIFT) & 0xff;
	return 0;
}

static void
imw_seticontitle(char *p)
{
	/*
		No separate icon-title concept under widget model.
		Functionally redundant with xsettitle.
	*/
	(void)p;
}

static void
imw_settitle(char *p)
{
	/*
		Store internally; term_draw_widget uses this in the ImGui
		Begin label via the ###id pattern.
	*/
	snprintf(term_title, sizeof term_title, "%s", p ? p : "Terminal");
}

static int
imw_setcursor(int cursor)
{
	if (!BETWEEN(cursor, 0, 7))
		return 1;
	tw.cursor = cursor;
	return 0;
}

static void
imw_setmode(int set, unsigned int flags)
{
	int prev = tw.mode;
	MODBIT(tw.mode, set, flags);
	/*  Mirror x.c:1739: full redraw on whole-screen-reverse-video flip.  */
	if ((tw.mode & MODE_REVERSE) != (prev & MODE_REVERSE))
		redraw();

	/*
		Defense for the mid-drag race: if a MODE_MOUSE* bit just flipped
		while a local selection was in progress, the eventual release will
		fire on the reporting path and never call selextend(done=1). Clear
		imw_selecting so the next local-mode click starts cleanly. Reset
		motion dedupe state too so the first motion after a mode change
		always emits (don't accidentally suppress because the new app's
		first cursor cell happens to match the last cell from the previous
		mode).
	*/
	if ((tw.mode & MODE_MOUSE) != (prev & MODE_MOUSE)) {
		imw_selecting = false;
		imw_last_motion_x = -1;
		imw_last_motion_y = -1;
	}
}

static void
imw_setpointermotion(int set)
{
	/*  No-op: ImGui always provides MousePos; nothing to toggle.  */
	(void)set;
}

static void
imw_setsel(char *str)
{
	/*  Collapse to OS clipboard via ImGui's API.  */
	if (str)
		ImGui::SetClipboardText(str);
}

static int
imw_startdraw(void)
{
	/*
		Clear the cursor overlay at the start of each draw cycle.
		If multiple draw() invocations happen in one frame (e.g.,
		redraw() from inside imw_setcolorname during ttyread, plus
		the main draw() in term_draw_widget), each cycle starts fresh
		and the final overlay reflects the most-recent xdrawcursor call.
	*/
	imw_overlay_ops.clear();
	return (tw.mode & MODE_VISIBLE) != 0;
}

static void
imw_ximspot(int x, int y)
{
	/*  No IME exposure.  */
	(void)x; (void)y;
}


/*
	Shortcut handlers — referenced by shortcuts[] / mshortcuts[] in
	config-imgui.h. These are the adapter-side counterparts to x.c's
	equivalents at config.def.h:259-331. The st.c-side handlers
	(sendbreak, printscreen, printsel, toggleprinter) live in st.c
	and are referenced directly by the arrays — no wrapper needed.
*/

/*  Copy current selection to OS clipboard. Mirrors x.c:259.  */
static void
clipcopy(const Arg *)
{
	char *sel = getsel();
	if (sel) {
		ImGui::SetClipboardText(sel);
		free(sel);
	}
}

/*
	Paste from OS clipboard, optionally wrapped in bracketed-paste markers
	if MODE_BRCKTPASTE is set. Mirrors x.c:274 + selnotify's bracketed
	paste wrapping (lines 590-595).
*/
static void
clippaste(const Arg *)
{
	const char *txt = ImGui::GetClipboardText();
	if (!txt || !*txt) return;
	size_t n = strlen(txt);
	if (tw.mode & MODE_BRCKTPASTE)
		ttywrite("\033[200~", 6, 0);
	ttywrite(txt, n, 0);   /*  may_echo=0 — paste, not typed  */
	if (tw.mode & MODE_BRCKTPASTE)
		ttywrite("\033[201~", 6, 0);
}

/*
	selpaste collapses to the same as clippaste (single OS clipboard,
	no PRIMARY/CLIPBOARD distinction). Mirrors x.c:284.
*/
static void
selpaste(const Arg *arg)
{
	clippaste(arg);
}

/*  Toggle MODE_NUMLOCK. Mirrors x.c:291 — note no redraw needed.  */
static void
numlock(const Arg *)
{
	tw.mode ^= MODE_NUMLOCK;
}

/*
	Send a literal byte sequence to the pty (e.g., scroll-wheel mappings,
	custom function-key sequences from mshortcuts/shortcuts).
*/
static void
ttysend(const Arg *arg)
{
	if (arg && arg->s)
		ttywrite(arg->s, strlen(arg->s), 1);   /*  may_echo=1 — keyboard-typed  */
}

/*
	Font zoom — relative (zoom) and absolute (zoomabs/zoomreset).
	Stub for now: full implementation requires unloading + reloading the
	4 ImGui font variants at the new size and re-deriving cell metrics.
*/
static void
zoom(const Arg *)
{
	/*  TODO: defer, then reload fonts at adjusted size.  */
}

static void
zoomabs(const Arg *)
{
	/*  TODO  */
}

static void
zoomreset(const Arg *)
{
	/*  TODO  */
}

/*
	Color parsing.

	Mirrors XParseColor for the formats actually used in real configs:
	  #rgb, #rrggbb, #rrrrggggbbbb, rgb:R/G/B, named.
	  Skips: rgbi:, CIE*: (never seen in practice).
	Named lookup uses rgb.txt at repo root (copy of X.Org's canonical
	file). Names are normalized (strip whitespace, lowercase) before
	both insertion and lookup, matching X11's case+space-insensitive
	comparison.
*/

static int
imw_hex_digit(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/*
	Parse n_digits hex chars to a 16-bit value via the X11 scaling rule:
	  value * 0xFFFF / ((1<<(n*4)) - 1)
	Identity at 4 digits; byte-replicate at 1-2 digits; correct for the
	rare 3-digit form. Caller does `>> 8` to get 8-bit.
	Returns true on success and writes the result through `out`. Returns
	false on any non-hex char without touching `out`. This matches the
	XParseColor contract (Status return + out-pointer for the value) —
	a sentinel return wouldn't work because 0xFFFF is also a legal max
	channel value (e.g., from "ff" or "f").
*/
static bool
imw_parse_hex_component(const char *s, int n_digits, uint16_t *out)
{
	int v = 0;
	for (int i = 0; i < n_digits; i++) {
		int d = imw_hex_digit(s[i]);
		if (d < 0) return false;
		v = (v << 4) | d;
	}
	int max = (1 << (n_digits * 4)) - 1;
	*out = (uint16_t)((unsigned long)v * 0xFFFF / (unsigned long)max);
	return true;
}

static std::string
imw_normalize_name(const char *name)
{
	std::string out;
	out.reserve(strlen(name));
	for (const char *p = name; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if (!isspace(c))
			out += (char)tolower(c);
	}
	return out;
}

static bool
imw_lookup_rgb(const char *name, uint8_t *r, uint8_t *g, uint8_t *b)
{
	std::string canonical = imw_normalize_name(name);
	auto it = std::lower_bound(imw_rgb_db.begin(), imw_rgb_db.end(), canonical,
	    [](const ColorEntry &e, const std::string &s) {
	        return e.canonical < s;
	    });
	if (it == imw_rgb_db.end() || it->canonical != canonical)
		return false;
	*r = it->r;
	*g = it->g;
	*b = it->b;
	return true;
}

static void
imw_get_exe_dir(char *out, size_t n)
{
	out[0] = '\0';
#ifdef _WIN32
	DWORD r = GetModuleFileNameA(NULL, out, (DWORD)n);
	if (r == 0 || r >= n)
		return;
	char *sep = strrchr(out, '\\');
	if (!sep) sep = strrchr(out, '/');
	if (sep) *sep = '\0';
	return;
#elif defined(__APPLE__)
	uint32_t sz = (uint32_t)n;
	if (_NSGetExecutablePath(out, &sz) != 0)
		return;
#elif defined(__linux__)
	ssize_t r = readlink("/proc/self/exe", out, n - 1);
	if (r <= 0)
		return;
	out[r] = '\0';
#endif
	char *slash = strrchr(out, '/');
	if (slash)
		*slash = '\0';
}

static void
imw_load_rgb_db(void)
{
	if (!imw_rgb_db.empty())
		return;

	/* Try <exe_dir>/../rgb.txt (binary in build/, rgb.txt at repo root),
	 * then <exe_dir>/rgb.txt (rgb.txt installed alongside the binary),
	 * then ./rgb.txt as a CWD fallback. */
	FILE *f = NULL;
	char dir[1024];
	imw_get_exe_dir(dir, sizeof dir);
	if (dir[0]) {
		char path[1024];
		snprintf(path, sizeof path, "%s/../rgb.txt", dir);
		f = fopen(path, "r");
		if (!f) {
			snprintf(path, sizeof path, "%s/rgb.txt", dir);
			f = fopen(path, "r");
		}
	}
	if (!f)
		f = fopen("rgb.txt", "r");
	if (!f)
		die("imgui_win: cannot open rgb.txt: %s\n", strerror(errno));
	char line[256];
	while (fgets(line, sizeof line, f)) {
		if (line[0] == '!' || line[0] == '#')
			continue;
		int r, g, b, n;
		if (sscanf(line, "%d %d %d %n", &r, &g, &b, &n) != 3)
			continue;
		char *name = line + n;
		/*  Trim trailing newline / cr.  */
		size_t len = strlen(name);
		while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r'))
			name[--len] = '\0';
		if (len == 0)
			continue;
		ColorEntry e;
		e.r = (uint8_t)r;
		e.g = (uint8_t)g;
		e.b = (uint8_t)b;
		e.canonical = imw_normalize_name(name);
		imw_rgb_db.push_back(std::move(e));
	}
	fclose(f);
	std::sort(imw_rgb_db.begin(), imw_rgb_db.end(),
	    [](const ColorEntry &a, const ColorEntry &b) {
	        return a.canonical < b.canonical;
	    });
}

/*
	parse_color: resolve any X-style color spec to 8-bit RGB.
	Returns true on success, false on parse failure.
*/
static bool
imw_parse_color(const char *name, uint8_t *r, uint8_t *g, uint8_t *b)
{
	if (!name)
		return false;

	/*  #rgb / #rrggbb / #rrrrggggbbbb  */
	if (name[0] == '#') {
		size_t hexlen = strlen(name + 1);
		if (hexlen != 3 && hexlen != 6 && hexlen != 12)
			return false;
		int per = (int)hexlen / 3;
		uint16_t r16, g16, b16;
		if (!imw_parse_hex_component(name + 1 + 0*per, per, &r16) ||
		    !imw_parse_hex_component(name + 1 + 1*per, per, &g16) ||
		    !imw_parse_hex_component(name + 1 + 2*per, per, &b16))
			return false;
		*r = (uint8_t)(r16 >> 8);
		*g = (uint8_t)(g16 >> 8);
		*b = (uint8_t)(b16 >> 8);
		return true;
	}

	/*  rgb:R/G/B with 1-4 hex per component.  */
	if (strncmp(name, "rgb:", 4) == 0) {
		const char *s = name + 4;
		const char *slash1 = strchr(s, '/');
		if (!slash1) return false;
		const char *slash2 = strchr(slash1 + 1, '/');
		if (!slash2) return false;
		int n_r = (int)(slash1 - s);
		int n_g = (int)(slash2 - slash1 - 1);
		int n_b = (int)strlen(slash2 + 1);
		if (n_r < 1 || n_r > 4 || n_g < 1 || n_g > 4 || n_b < 1 || n_b > 4)
			return false;
		uint16_t r16, g16, b16;
		if (!imw_parse_hex_component(s,          n_r, &r16) ||
		    !imw_parse_hex_component(slash1 + 1, n_g, &g16) ||
		    !imw_parse_hex_component(slash2 + 1, n_b, &b16))
			return false;
		*r = (uint8_t)(r16 >> 8);
		*g = (uint8_t)(g16 >> 8);
		*b = (uint8_t)(b16 >> 8);
		return true;
	}

	/*  Named lookup against rgb.txt.  */
	return imw_lookup_rgb(name, r, g, b);
}

/*
	sixd_to_8bit: 256-color cube intensity scaling. Mirrors x.c::sixd_to_16bit
	(line 765) followed by `>> 8`. {0, 95, 135, 175, 215, 255} for x ∈ [0,5].
*/
static uint8_t
imw_sixd_to_8bit(int x)
{
	if (x == 0) return 0;
	return (uint8_t)((0x3737 + 0x2828 * x) >> 8);
}

/*
	Resolve one palette slot (mirrors x.c::xloadcolor logic flow):
	  - explicit name (non-NULL): parse via imw_parse_color
	  - NULL name + i in [16,255]: derive from 256-color cube / grayscale ramp
	  - NULL name + i in [0,15] or [256,259]: use colorname_low/high entry

	Mirrors config.def.h's static colorname[] (lines 97-125). The colorname
	tables stay here temporarily; migrate to config-imgui.def.h with the rest
	of the config stubs.

	Shared by xloadcols (initial population) and xsetcolorname (OSC dynamic
	update + OSC reset-to-default with name=NULL). The shared path is what
	makes `xsetcolorname(i, NULL)` correctly reset to the per-index default
	instead of failing — matching x.c lines 833-843 + 771-792 verbatim.
*/
static bool
imw_resolve_color_at(int i, const char *name, ImU32 *out)
{
	/*  colorname_low / colorname_high come from config-imgui.h.  */
	uint8_t r = 0, g = 0, b = 0;

	if (!name) {
		/*  No explicit name — match x.c xloadcolor 775-789.  */
		if (BETWEEN(i, 16, 255)) {
			if (i < 6*6*6 + 16) {
				r = imw_sixd_to_8bit(((i - 16) / 36) % 6);
				g = imw_sixd_to_8bit(((i - 16) / 6)  % 6);
				b = imw_sixd_to_8bit(((i - 16) / 1)  % 6);
			} else {
				int gray16 = 0x0808 + 0x0a0a * (i - (6*6*6 + 16));
				r = g = b = (uint8_t)(gray16 >> 8);
			}
			*out = IM_COL32(r, g, b, 255);
			return true;
		}
		if (i >= 0 && i < 16)
			name = colorname_low[i];
		else if (i >= 256 && i < 260)
			name = colorname_high[i - 256];
		else
			return false;
	}

	if (!imw_parse_color(name, &r, &g, &b))
		return false;
	*out = IM_COL32(r, g, b, 255);
	return true;
}

/*
	Glyph rendering (xclear + xdrawglyphfontspecs)

	The full attribute pipeline is implemented:
	  - Plain glyphs with indexed fg/bg, no attributes
	  - ATTR_REVERSE per-cell (swap fg/bg)
	  - Truecolor path
	  - ATTR_BOLD/ITALIC font selection (in xmakeglyphfontspecs)
	  - ATTR_FAINT dimming
	  - ATTR_BLINK + MODE_BLINK (with timer in term_draw_widget)
	  - ATTR_INVISIBLE / ATTR_UNDERLINE / ATTR_STRUCK
	  - Bold-color promotion (indexed [0,7] + bold -> [8,15])
	  - badweight/badslant override
	  - MODE_REVERSE whole-screen
	  - Border clearing at canvas edges
	  - Selection-inversion path: in xdrawline
	    (`if (selected(x, y)) neu.mode ^= ATTR_REVERSE;` mirrors x.c)
*/

/*
	xclear — fill a widget-relative rect with a resolved ImU32. Mirrors
	x.c::xclear (line 853) which is just XftDrawRect(...). Caller does the
	color-index lookup; this helper is intentionally dumb.
*/
static void
imw_clear(int x1, int y1, int x2, int y2, ImU32 col)
{
	imw_emit_rect(ImVec2((float)x1, (float)y1),
	              ImVec2((float)x2, (float)y2), col);
}

/*
	Resolve a glyph fg/bg field to an ImU32 — handles both truecolor (bit-24
	set, low 24 bits hold packed RGB) and indexed (palette lookup). Mirrors
	x.c lines 1397-1417's IS_TRUECOL branch.
*/
static inline ImU32
imw_resolve_glyph_color(uint32_t c)
{
	if (IS_TRUECOL(c))
		return IM_COL32((c >> 16) & 0xff,
		                (c >>  8) & 0xff,
		                 c        & 0xff, 0xff);
	return colors[c];
}

/*
	Per-frame blink timer. Mirrors x.c::run lines 2011-2020: every
	blinktimeout ms, if any cell has ATTR_BLINK, toggle MODE_BLINK. Single
	timer drives both per-cell text blink and (eventually) cursor blink.
*/
static void
imw_tick_blink(void)
{
	static double last_toggle_ms = 0.0;
	double now_ms = ImGui::GetTime() * 1000.0;
	if (now_ms - last_toggle_ms < (double)blinktimeout)
		return;
	if (tattrset(ATTR_BLINK)) {
		/*
			Mirror x.c::run line 2017: toggle MODE_BLINK then dirty
			every row that has blinking content so xdrawline re-emits
			with the new visibility. Without the tsetdirtattr call,
			cached row ops keep the prior fg=bg (or fg≠bg) state baked
			in and the visual blink stops under draw()-only mode.
		*/
		tw.mode ^= MODE_BLINK;
		tsetdirtattr(ATTR_BLINK);
	} else {
		/*
			Nothing blinking — keep MODE_BLINK in the "on" state so the
			next time something starts blinking it begins visible.
		*/
		tw.mode |= MODE_BLINK;
	}
	last_toggle_ms = now_ms;
}

/*
	Modifier match — mirrors x.c::match (line 1800).
	  IMW_MOD_ANY:  match regardless of state
	  else:         exact equality after stripping ignoremod from state
	Used by the kmap and shortcut dispatchers.
*/
static inline bool
imw_match_mods(ImGuiKeyChord wanted, int got)
{
	if (wanted == IMW_MOD_ANY)
		return true;
	return wanted == (got & ~ignoremod);
}

/*
	Screen pixel -> terminal cell. Mirrors x.c::evcol/evrow (lines 333-346):
	subtract canvas origin and borderpx, clamp to grid pixel extent, divide
	by cell metrics. Clamping (vs rejecting out-of-range) matches x.c so a
	drag past the right/bottom edge pins to the last column/row instead of
	silently dropping the extend.
*/
static inline void
imw_pixel_to_cell(ImVec2 p, int *col, int *row)
{
	int x = (int)(p.x - imw_ctx.canvas_pos.x) - borderpx;
	int y = (int)(p.y - imw_ctx.canvas_pos.y) - borderpx;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > tw.tw - 1) x = tw.tw - 1;
	if (y > tw.th - 1) y = tw.th - 1;
	*col = x / tw.cw;
	*row = y / tw.ch;
}

/*
	Mouse-report encoder — default 6-byte format. Mirrors x.c::mousereport
	lines 422-433. Layout is `\033[M` + 3 bytes (button, col, row), each
	coordinate `cell + 1 + 32` (1-based, +32 to keep printable).

	Trap 1: 223-cell cap. `255 - 32 - 1 = 222` is the largest cell that
	fits in a uint8_t after the +33 offset, so cells 223+ overflow.
	Upstream clamps to 222 (1-based: cell index 222 -> byte 0xff). Match
	upstream literally — non-SGR apps on wide terminals will see
	wrong-cell reports past col/row 222; SGR is the supported workaround.

	`release` flag: in the default format, release is encoded as button
	code 3 regardless of which button was released. SGR preserves
	the button code and distinguishes via the `M`/`m` suffix.

	`mods` is the pre-OR'd modifier bits (Shift=4, Alt=8, Ctrl=16). Caller
	skips this under MODE_MOUSEX10.
*/
static void
imw_emit_mouse_default(int btn_code, int col, int row, int mods, bool release)
{
	if (release)
		btn_code = 3;
	int code = btn_code + mods;

	/*  +1 for 1-based cells, then clamp so cell+1 ≤ 223 (byte ≤ 0xff).  */
	int cx = col + 1, cy = row + 1;
	if (cx > 223) cx = 223;
	if (cy > 223) cy = 223;
	if (cx < 1)   cx = 1;
	if (cy < 1)   cy = 1;

	char buf[6];
	buf[0] = '\033';
	buf[1] = '[';
	buf[2] = 'M';
	buf[3] = (char)(32 + code);
	buf[4] = (char)(32 + cx);
	buf[5] = (char)(32 + cy);
	ttywrite(buf, 6, 0);  /*  may_echo=0: protocol bytes, not typed input  */
}

/*
	Mouse-report encoder — SGR (1006) format. Mirrors x.c::mousereport
	lines 423-426. Layout is `\033[<code;col;row;M` for press / motion,
	`\033[<code;col;row;m` for release. Decimal text, so no 223-cell cap.

	Release semantics differ from the default-format encoder: SGR
	preserves the button code and distinguishes via the M/m suffix.
	Default format rewrites btn_code to 3 on release; SGR doesn't.

	Modifier bits (Shift=4, Alt=8, Ctrl=16) merge in the same way as
	the default format. X10-mode skip happens at the caller.
*/
static void
imw_emit_mouse_sgr(int btn_code, int col, int row, int mods, bool release)
{
	int code = btn_code + mods;
	int cx = col + 1, cy = row + 1;
	if (cx < 1) cx = 1;
	if (cy < 1) cy = 1;

	char buf[32];
	int n = snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c",
	                 code, cx, cy, release ? 'm' : 'M');
	if (n > 0 && n < (int)sizeof buf)
		ttywrite(buf, (size_t)n, 0);
}

/*
	Format-aware dispatcher. MODE_MOUSESGR is orthogonal to the protocol
	level (X10/BTN/MOTION/MANY) — an app enables SGR by setting both
	MODE_MOUSEBTN (or higher) AND MODE_MOUSESGR. Caller has already
	verified report_mouse; this only chooses the wire format.
*/
static void
imw_emit_mouse(int btn_code, int col, int row, int mods, bool release)
{
	if (tw.mode & MODE_MOUSESGR)
		imw_emit_mouse_sgr(btn_code, col, row, mods, release);
	else
		imw_emit_mouse_default(btn_code, col, row, mods, release);
}

/*
	Per-frame non-blocking pump of the pty master fd. Six-line scaffolding
	— equivalent to x.c::run's select() loop minus the blocking part, since
	ImGui drives our frame cadence at vsync regardless of fd readiness.
	Reasonable for now; a background-read-thread variant is an option
	if latency becomes an issue.
*/
static void
imw_pump_pty(void)
{
	if (imw_cmdfd < 0)
		return;

	/*
		Drain the PTY until empty, time budget exhausted, or signal hit.
		One ttyread() per frame is too slow for high-volume output (cat
		of a big file, vim full-screen redraw); a fixed iteration cap
		hides saturation and gives a constant per-frame cost. Time budget
		degrades gracefully: under heavy bursts we bail when we've
		consumed our slice of the frame, leaving the rest of the work
		(input dispatch, draw, render) headroom and picking up the
		backlog next frame.

		~5ms target: at 60Hz the frame budget is ~16ms; the renderer
		and ImGui itself need most of it. select() with a zero timeout
		makes each iteration a non-blocking poll. EINTR is benign —
		retry rather than treat a stray signal as "no data".
	*/
	const double drain_budget_s = 0.005;
	const double deadline = ImGui::GetTime() + drain_budget_s;
#ifdef _WIN32
	/* Windows: poll the ConPTY read-pipe for available data. */
	for (;;) {
		DWORD avail = 0;
		if (!PeekNamedPipe(w32_pipe_in, NULL, 0, NULL, &avail, NULL))
			break;
		if (avail == 0)
			break;
		ttyread();
		if (ImGui::GetTime() >= deadline)
			break;
	}
#else
	fd_set rfds;
	struct timeval tv = { 0, 0 };
	for (;;) {
		FD_ZERO(&rfds);
		FD_SET(imw_cmdfd, &rfds);
		int n = select(imw_cmdfd + 1, &rfds, NULL, NULL, &tv);
		if (n < 0) {
			if (errno == EINTR) continue;
			break;
		}
		if (n == 0 || !FD_ISSET(imw_cmdfd, &rfds))
			break;
		ttyread();
		if (ImGui::GetTime() >= deadline)
			break;
	}
#endif
}

/*
	xmakeglyphfontspecs — pack a row of Glyphs into render-ready specs.
	Mirrors x.c lines 1247+. ATTR_BOLD/ATTR_ITALIC pick which
	dc.{font,bfont,ifont,ibfont} variant to use for the run. spec.src
	points at the chosen variant so xdrawglyphfontspecs can read
	src->badweight / src->badslant later without re-deriving.

	No FRC fallback yet — runes not in the primary variant emit a spec with
	the primary's ImFont*; ImGui's atlas substitutes its replacement glyph.
	FRC support lands when the runtime-FRC analog is implemented.
*/
static int
imw_makeglyphfontspecs(ImwGlyphSpec *specs, const Glyph *glyphs,
                       int len, int x, int y)
{
	int winx = borderpx + x * tw.cw;
	int winy = borderpx + y * tw.ch;
	int xp   = winx;
	int numspecs = 0;

	for (int i = 0; i < len; i++) {
		Rune    rune = glyphs[i].u;
		ushort  mode = glyphs[i].mode;

		/*  Wide-char dummy second cell: skip entirely.  */
		if (mode == ATTR_WDUMMY)
			continue;

		/*
			Pick font variant by ATTR_BOLD|ATTR_ITALIC.
			Mirrors x.c lines 1277-1286.
		*/
		Font *fnt = &dc.font;
		if ((mode & ATTR_BOLD_FAINT) == ATTR_BOLD && (mode & ATTR_ITALIC))
			fnt = &dc.ibfont;
		else if ((mode & ATTR_BOLD_FAINT) == ATTR_BOLD)
			fnt = &dc.bfont;
		else if (mode & ATTR_ITALIC)
			fnt = &dc.ifont;

		int runewidth = tw.cw * ((mode & ATTR_WIDE) ? 2 : 1);

		if (numspecs >= IMW_SPECS_BUFLEN) {
			fprintf(stderr,
			    "imgui_win: spec buffer overflow at row %d (cap %d)\n",
			    y, IMW_SPECS_BUFLEN);
			break;
		}

		specs[numspecs].font      = fnt->match;
		specs[numspecs].src       = fnt;
		specs[numspecs].codepoint = rune;
		specs[numspecs].x         = xp;
		specs[numspecs].y         = winy;

		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

/*
	xdrawglyphfontspecs — render a run of cells sharing (mode, fg, bg).
	Resolves indexed + truecolor fg/bg, applies ATTR_FAINT, ATTR_REVERSE
	per-cell, MODE_REVERSE whole-screen, ATTR_BLINK, ATTR_INVISIBLE,
	ATTR_UNDERLINE, ATTR_STRUCK, badweight/badslant override, and
	bold-color promotion.
*/
static void
imw_drawglyphfontspecs(const ImwGlyphSpec *specs, Glyph base,
                       int len, int x, int y)
{
	int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);
	int winx    = borderpx + x * tw.cw;
	int winy    = borderpx + y * tw.ch;
	int width   = charlen * tw.cw;

	/*
		Resolve fg/bg through the indexed-or-truecolor branch.
		Mirrors x.c lines 1397-1417's IS_TRUECOL split for both fg and bg.
	*/
	ImU32 fg = imw_resolve_glyph_color(base.fg);
	ImU32 bg = imw_resolve_glyph_color(base.bg);

	/*
		Badweight/badslant override. When fontconfig fell back to a
		non-matching variant for this run's attribute set, the spec
		packer flagged the variant via src->badweight/badslant. Render
		with a distinctive color (defaultattr, typically yellow) as
		visual feedback that the requested style isn't really present.
		Mirrors x.c lines 1390-1394.

		All glyphs in a run share the same src (xmakeglyphfontspecs picks
		variant per mode; mode is constant within a run by ATTRCMP), so
		specs[0].src is the single source of truth. xdrawline guarantees
		len ≥ 1 — this function is never called with an empty run.
	*/
	if (specs[0].src && (specs[0].src->badslant || specs[0].src->badweight))
		fg = colors[defaultattr];

	/*
		Bold-color promotion. Indexed fg in [0,7] with ATTR_BOLD set and
		ATTR_FAINT clear -> use bright variant [8,15]. Mirrors x.c lines
		1419-1421. Equality form `== ATTR_BOLD` excludes BOLD+FAINT
		(faint wins by virtue of the equality). Truecolor is automatically
		excluded — TRUECOLOR values have bit 24 set, well above 7, so the
		BETWEEN check fails for them. No redundant !IS_TRUECOL guard needed.
	*/
	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD
	    && BETWEEN((int)base.fg, 0, 7)) {
		fg = colors[base.fg + 8];
	}

	/*
		MODE_REVERSE whole-screen. Mirrors x.c lines 1424-1446.
		If the cell's color matches the default slot, swap defaults
		(defaultfg ↔ defaultbg) for a clean inversion that preserves
		"default text" appearance. Otherwise bitwise-invert the channels.
		The XOR shortcut `(c ^ 0x00ffffff) | 0xff000000` matches per-channel
		`~r & 0xff` exactly while sidestepping the `~r` sign-extension
		trap. Whole-screen MODE_REVERSE is distinct from the per-cell
		ATTR_REVERSE handled below — both can stack.
	*/
	if (tw.mode & MODE_REVERSE) {
		if (fg == colors[defaultfg])
			fg = colors[defaultbg];
		else
			fg = (fg ^ 0x00ffffff) | 0xff000000;
		if (bg == colors[defaultbg])
			bg = colors[defaultfg];
		else
			bg = (bg ^ 0x00ffffff) | 0xff000000;
	}

	/*
		ATTR_FAINT — halve fg RGB channels (fg only, not bg).
		Equality form (mode & ATTR_BOLD_FAINT) == ATTR_FAINT excludes the
		BOLD+FAINT combo — bold takes precedence there. Mirrors x.c
		lines 1450-1455. The 8-bit halve is exact-match to x.c's
		16-bit halve+>>8.
	*/
	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		uint8_t r = (fg >> IM_COL32_R_SHIFT) & 0xff;
		uint8_t g = (fg >> IM_COL32_G_SHIFT) & 0xff;
		uint8_t b = (fg >> IM_COL32_B_SHIFT) & 0xff;
		fg = IM_COL32(r / 2, g / 2, b / 2, 0xff);
	}

	/*
		ATTR_REVERSE per-cell — swap fg and bg. Mirrors x.c line 1460.
		Note: the WHOLE-SCREEN MODE_REVERSE inversion is a separate
		thing handled above; this is just the per-cell attribute.
	*/
	if (base.mode & ATTR_REVERSE) {
		ImU32 t = fg; fg = bg; bg = t;
	}

	/*
		ATTR_BLINK + MODE_BLINK — both required (mirrors x.c:1464).
		MODE_BLINK is the global toggle driven by the blink timer in
		term_draw_widget; ATTR_BLINK is the per-cell flag. Setting fg = bg
		makes the glyph invisible during the off-phase (text "blinks").
	*/
	if ((base.mode & ATTR_BLINK) && (tw.mode & MODE_BLINK))
		fg = bg;

	/*  ATTR_INVISIBLE — unconditional hide. Mirrors x.c:1467.  */
	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	/*
		Border clearing at canvas edges. Mirrors x.c lines 1471-1483.
		When a cell sits at any of the 4 outer edges of the
		terminal raster, extend the cell's bg color into the gutter
		(the borderpx-wide strip between the raster and the canvas edge).
		Without this, gutters at the edges keep the canvas-wide default
		bg and don't follow per-cell bg variations. Done BEFORE the cell
		bg fill so the gutter and cell agree.
	*/
	if (x == 0)  /*  left gutter  */
		imw_clear(0,
		           (y == 0) ? 0 : winy,
		           borderpx,
		           winy + tw.ch, bg);
	if (winx + width >= borderpx + tw.tw)  /*  right gutter  */
		imw_clear(winx + width,
		           (y == 0) ? 0 : winy,
		           tw.w,
		           winy + tw.ch, bg);
	if (y == 0)  /*  top gutter  */
		imw_clear(winx, 0, winx + width, borderpx, bg);
	if (winy + tw.ch >= borderpx + tw.th)  /*  bottom gutter  */
		imw_clear(winx, winy + tw.ch, winx + width, tw.h, bg);

	/*  Cell bg fill — coords widget-relative; replay adds canvas_pos.  */
	ImVec2 p0((float)winx, (float)winy);
	ImVec2 p1((float)(winx + width), (float)(winy + tw.ch));
	imw_emit_rect(p0, p1, bg);

	/*
		Clip glyphs to the cell rect — keeps italic overhang from bleeding
		into adjacent cells.
	*/
	imw_emit_push_clip(p0, p1);

	/*
		Emit each glyph. spec coords are widget-relative.
		No defensive checks on s->font / s->codepoint — malformed specs
		are bugs we want to hear about loudly, not skip silently.
		ATTR_WDUMMY cells are filtered upstream by the spec packer; they
		never reach this loop.
	*/
	for (int i = 0; i < len; i++) {
		const ImwGlyphSpec *s = &specs[i];
		char buf[8];
		int n = (int)utf8encode(s->codepoint, buf);
		ImVec2 gp((float)s->x, (float)s->y);
#ifdef _WIN32
		/* Center emoji glyphs in their cell. ImGui renders at
		   pos + (X0,Y0)*scale — shift pos so the glyph visual
		   center lands at the cell center. Mirrors a quirk Windows
		   Terminal also fails to handle: it doesn't center emoji
		   glyphs properly inside their wide-cell slot. */
		if ((base.mode & ATTR_WIDE) && s->codepoint > 0xFFFF) {
			ImFontBaked *baked = s->font->GetFontBaked(dc.font.pxsize);
			if (baked) {
				const ImFontGlyph *gl = baked->FindGlyph((ImWchar)s->codepoint);
				if (gl) {
					float sc = dc.font.pxsize / baked->Size;
					float gw = (gl->X1 - gl->X0) * sc;
					float gh = (gl->Y1 - gl->Y0) * sc;
					float cw2 = (float)(tw.cw * 2);
					float ch1 = (float)tw.ch;
					gp.x += (cw2 - gw) * 0.5f - gl->X0 * sc;
					gp.y += (ch1 - gh) * 0.5f - gl->Y0 * sc;
				}
			}
		}
#endif
		imw_emit_text(s->font, gp, fg, buf, n);
	}

	/*
		ATTR_UNDERLINE — 1px line below baseline. Mirrors x.c:1500.
		Uses dc.font.ascent (regular variant only) for consistency across
		mixed-variant runs — avoids baseline jumps.
	*/
	if (base.mode & ATTR_UNDERLINE) {
		float uy = (float)(winy + dc.font.ascent + 1);
		imw_emit_rect(ImVec2(p0.x, uy),
		              ImVec2(p1.x, uy + 1.0f), fg);
	}

	/*
		ATTR_STRUCK — 1px line at 2/3 ascent height. Mirrors x.c:1505.
		Same regular-variant rationale as underline.
	*/
	if (base.mode & ATTR_STRUCK) {
		float sy = (float)winy + 2.0f * (float)dc.font.ascent / 3.0f;
		imw_emit_rect(ImVec2(p0.x, sy),
		              ImVec2(p1.x, sy + 1.0f), fg);
	}

	imw_emit_pop_clip();
}

/*
	xdrawglyph — single-glyph wrapper. Equivalent to x.c::xdrawglyph
	(line 1514): pack one glyph into a one-element spec array, dispatch
	through the same render path as xdrawline does for runs of glyphs.
	Used by xdrawcursor for the block-style render and the old-cursor
	erase.
*/
static void
imw_drawglyph(Glyph g, int x, int y)
{
	ImwGlyphSpec spec;
	int numspecs = imw_makeglyphfontspecs(&spec, &g, 1, x, y);
	if (numspecs > 0)
		imw_drawglyphfontspecs(&spec, g, numspecs, x, y);
}

/*
	Font loading.

	Pipeline (port-for-port from x.c lines 985-1052):
	  FcNameParse(font_pattern_str)
	      -> FcConfigSubstitute + FcDefaultSubstitute
	      -> FcFontMatch
	      -> FcPatternGetString(match, FC_FILE) yields TTF path
	      -> AddFontFromFileTTF(path, size, &cfg)
	Pattern is mutated in place to derive italic / bold-italic / bold;
	mutation order matches x.c exactly because pattern state carries.

	Missing variants: fontconfig falls back to a non-matching font and
	we set badslant/badweight on the variant. xdrawglyphfontspecs
	observes those flags and triggers the defaultattr color override.
*/

static int
load_one_font(Font *f, FcPattern *src_pattern, double pxsize)
{
	memset(f, 0, sizeof(*f));
	f->pxsize = (float)pxsize;

	/*  Duplicate so caller's pattern can keep being mutated for next variant.  */
	FcPattern *p = FcPatternDuplicate(src_pattern);
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, p, &result);
	if (!match) {
		FcPatternDestroy(p);
		fprintf(stderr, "imgui_win: FcFontMatch failed\n");
		return 0;
	}

	FcChar8 *file = NULL;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
		FcPatternDestroy(match);
		FcPatternDestroy(p);
		fprintf(stderr, "imgui_win: matched font has no FC_FILE\n");
		return 0;
	}

	/*  Detect badslant/badweight by comparing requested vs matched.  */
	int wantv, gotv;
	if (FcPatternGetInteger(p, FC_SLANT, 0, &wantv) == FcResultMatch
	 && FcPatternGetInteger(match, FC_SLANT, 0, &gotv) == FcResultMatch
	 && wantv != gotv) {
		f->badslant = 1;
	}
	if (FcPatternGetInteger(p, FC_WEIGHT, 0, &wantv) == FcResultMatch
	 && FcPatternGetInteger(match, FC_WEIGHT, 0, &gotv) == FcResultMatch
	 && wantv != gotv) {
		f->badweight = 1;
	}

	ImFontConfig cfg;
	cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor;
	f->match = ImGui::GetIO().Fonts->AddFontFromFileTTF(
	    (const char *)file, (float)pxsize, &cfg);

	FcPatternDestroy(match);
	FcPatternDestroy(p);

	if (!f->match) {
		fprintf(stderr, "imgui_win: AddFontFromFileTTF failed\n");
		return 0;
	}
	return 1;
}

/*
	Load a fallback font and merge its glyphs into the most-recently-
	added ImFont's index. ImGui's MergeMode=true appends into the
	previous font's IndexLookup table, so this MUST be called immediately
	after the primary variant we want to extend.

	"FRC v1": static merge at startup. Mirrors x.c's frc[] mechanism but
	resolved once up front instead of per-rune at draw time. The v2
	(per-rune dynamic discovery + atlas rebuild) would handle rare scripts
	not in our two fallback fonts; deferred — covers <5% of real use.

	Discovery via fontconfig keeps this cross-platform: same code finds
	Hiragino Sans on macOS and Noto CJK on Linux. No hardcoded paths.

	Failure is non-fatal: minimal systems may not have CJK or emoji fonts
	installed; the terminal still launches and missing glyphs render as
	the replacement char. Don't die().

	LoadColor is unconditional. Mono fallbacks (e.g., Hiragino Sans)
	ignore it; color emoji fonts (Apple Color Emoji, Noto Color Emoji)
	honor it via FreeType's sbix/COLR support.
*/
static void
imw_load_fallback(const char *fc_query, double pxsize)
{
	FcPattern *pat = FcNameParse((const FcChar8 *)fc_query);
	if (!pat) {
		fprintf(stderr, "imgui_win: fallback FcNameParse failed: %s\n",
		        fc_query);
		return;
	}
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	FcPatternDestroy(pat);
	if (!match) {
		fprintf(stderr, "imgui_win: no fallback for '%s'\n", fc_query);
		return;
	}

	FcChar8 *file = NULL;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch
	    || !file) {
		FcPatternDestroy(match);
		fprintf(stderr, "imgui_win: fallback '%s' has no FC_FILE\n",
		        fc_query);
		return;
	}

	ImFontConfig cfg;
	cfg.MergeMode = true;
	/*
		LoadColor: COLR/CPAL vector color glyphs (Microsoft, Twemoji,
		           Noto Color Emoji's modern format).
		Bitmap:    sbix and CBDT bitmap-emoji tables (Apple Color Emoji,
		           Google's older Noto bitmap, Samsung Color Emoji).
		Both are needed — they're orthogonal codepaths in FreeType, so
		setting only LoadColor leaves Apple Color Emoji rendering as
		empty squares (sbix tables ignored).
	*/
	cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor
	                    | ImGuiFreeTypeLoaderFlags_Bitmap;

	/*
		Color-emoji size workaround: bitmap-emoji fonts (sbix in Apple
		Color Emoji, CBDT in Noto Color Emoji bitmap, etc.) ship a
		fixed set of pre-rendered strikes — they CANNOT be rasterized
		at arbitrary sizes. ImGui's loader uses FT_SIZE_REQUEST_TYPE_NOMINAL
		which fails (FT_Err_Invalid_Pixel_Size = 0x17) when the requested
		px size doesn't match any strike's ppem. At our primary 16px,
		Apple Color Emoji's smallest strike doesn't match -> load fails
		silently -> ImGui renders the FallbackChar (the `?` in a box).

		Fix: bump RasterizerDensity so the rasterizer-side request lands
		on a working strike, while the displayed glyph still fits the
		primary's cell. The math:
		  request_px  = pxsize × rasterizer_density × baked_density
		  display_px  = bitmap_w × (1 / rasterizer_density × baked_density)
		With density = 20/pxsize, request becomes 20 (= a working strike
		empirically tested across Apple Color Emoji's strike set), and
		display width stays bitmap_w × pxsize/20 ≈ pxsize. On Retina
		(baked_density=2), request becomes 40 — also a working strike.

		Only set this for the emoji query — CJK/Hiragino is an outline
		font and rasterizes correctly at any size, no override needed.
	*/
#ifndef _WIN32
	/* Apple Color Emoji is a bitmap font with fixed strikes — needs
	   density override to land on a working ppem. Windows emoji fonts
	   (Segoe UI Emoji, Noto Color Emoji) are outline/COLR and
	   rasterize correctly at any size without this hack. */
	if (strstr(fc_query, "und-zsye") != NULL)
		cfg.RasterizerDensity = 20.0f / (float)pxsize;
#endif

	ImFont *merged = ImGui::GetIO().Fonts->AddFontFromFileTTF(
	    (const char *)file, (float)pxsize, &cfg);
	if (!merged)
		fprintf(stderr, "imgui_win: fallback merge failed: %s (file=%s)\n",
		        fc_query, (const char *)file);

	FcPatternDestroy(match);
}

/*
	Per-variant fallback set. Called after each successful primary variant
	so bold-CJK / italic-emoji etc. render correctly via the matching
	variant's merged glyph map (instead of falling through to the
	replacement char). 4× atlas cost for fallback glyphs is acceptable;
	the alternative (single-merge into regular + per-glyph variant
	routing in xmakeglyphfontspecs) is the v2 complexity we're skipping.
*/
static void
imw_load_variant_fallbacks(double pxsize)
{
	imw_load_fallback(":lang=ja",       pxsize);  /*  CJK Unified Ideographs (ja covers shared zh/ko ranges)  */
	imw_load_fallback(":lang=und-zsye", pxsize);  /*  Unicode color emoji  */
}

static void
imw_load_fonts(void)
{
	if (!FcInit())
		die("imgui_win: FcInit failed\n");

	/*
		Activate the FreeType font loader BEFORE any AddFontFromFileTTF.
		Defining IMGUI_ENABLE_FREETYPE in imgui_user_config.h only
		compiles the FreeType code path; activation is a separate
		runtime call (changed in ImGui 1.92 — used to be implicit when
		the macro was defined, now explicit via SetFontLoader).

		Without this, the atlas silently falls back to stb_truetype,
		which (a) produces noticeably softer / blurrier glyphs at the
		sizes terminals use and (b) has no support for COLR/sbix tables
		so color emoji renders as monochrome silhouettes (or not at all).
	*/
	ImGui::GetIO().Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());

	FcPattern *pattern = FcNameParse((const FcChar8 *)font);
	if (!pattern)
		die("imgui_win: failed to parse font pattern: %s\n", font);

	double pxsize = 16.0;
	FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pxsize);

	/*
		Variant order matches x.c lines 1037-1048: regular, italic,
		bold-italic, bold. Pattern is mutated in place between calls.
		Fallbacks merge after each primary so all 4 variants share
		coverage; only call them when the primary actually loaded
		(otherwise the merge would target the wrong ImFont).
	*/
	if (load_one_font(&dc.font, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (load_one_font(&dc.ifont, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (load_one_font(&dc.ibfont, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (load_one_font(&dc.bfont, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDestroy(pattern);
}

static void
imw_derive_metrics_from_baked(Font *f)
{
	if (!f->match)
		return;
	ImFontBaked *baked = f->match->GetFontBaked(f->pxsize);
	if (!baked)
		return;
	f->ascent  = (int)ceilf(baked->Ascent);
	f->descent = (int)ceilf(-baked->Descent);  /*  Descent is typically negative  */
	/*
		Match x.c (line 953-ish): height = ascent + descent, NOT the
		nominal pixel size. The font's natural ascent+descent might exceed
		the requested px size; cell rows are sized to the natural extent
		to avoid clipping descenders.
	*/
	f->height  = f->ascent + f->descent;

	/*
		Width: lookup a real glyph, not FallbackAdvanceX (which requires the
		fallback char to have been rendered first). For monospace fonts all
		ASCII printables share the same advance — pick 'M' as canonical.
	*/
	float advance = baked->GetCharAdvance((ImWchar)'M');
	if (advance <= 0.0f)
		advance = baked->FallbackAdvanceX;
	if (advance <= 0.0f)
		advance = (float)f->height * 0.6f;  /*  last-resort heuristic  */
	f->width = (int)ceilf(advance);
}

/*
	Called once after the first ImGui frame, when the atlas has been baked
	and ImFontBaked is populated. Derives cell metrics from the regular
	variant only — bold/italic might have slightly different intrinsic
	metrics but the cell grid stays locked to regular.
*/
static void
imw_finalize_metrics(void)
{
	imw_derive_metrics_from_baked(&dc.font);
	imw_derive_metrics_from_baked(&dc.bfont);
	imw_derive_metrics_from_baked(&dc.ifont);
	imw_derive_metrics_from_baked(&dc.ibfont);

	/*  Cell metrics from regular variant only.  */
	if (dc.font.width > 0 && dc.font.height > 0) {
		tw.cw = (int)ceilf(dc.font.width  * cwscale);
		tw.ch = (int)ceilf(dc.font.height * chscale);
	}
	dc.metrics_derived = true;

	/*  Font-load diagnostic — commented out for clean console.
	    Uncomment to dump cell metrics + slot warnings at startup.

	fprintf(stderr,
	    "imgui_win: fonts loaded. "
	    "regular ascent=%d descent=%d w=%d h=%d  ->  cw=%d ch=%d  "
	    "(bold:badslant=%d badweight=%d, italic:badslant=%d badweight=%d, "
	    "bold-italic:badslant=%d badweight=%d)\n",
	    dc.font.ascent, dc.font.descent, dc.font.width, dc.font.height,
	    tw.cw, tw.ch,
	    dc.bfont.badslant, dc.bfont.badweight,
	    dc.ifont.badslant, dc.ifont.badweight,
	    dc.ibfont.badslant, dc.ibfont.badweight);
	*/
}

/*
	Per-frame resize detection. If the canvas has changed cell
	dimensions since the last frame, run the cresize path: tresize
	updates st.c's internal grid, ttyresize sends TIOCSWINSZ to the
	child so vim/less/htop relayout. Mirrors x.c::cresize line 745.
*/
static void
imw_handle_resize(ImVec2 avail)
{
	int new_cols = (int)avail.x / tw.cw;
	int new_rows = (int)avail.y / tw.ch;
	if (new_cols < 1) new_cols = 1;
	if (new_rows < 1) new_rows = 1;
	int cur_cols = tw.tw / tw.cw;
	int cur_rows = tw.th / tw.ch;
	if (new_cols == cur_cols && new_rows == cur_rows)
		return;

	tw.tw = new_cols * tw.cw;
	tw.th = new_rows * tw.ch;
	tw.w  = (int)avail.x;
	tw.h  = (int)avail.y;
	tresize(new_cols, new_rows);

	/*
		ttyresize must come AFTER tresize since it reads
		term.row/term.col internally to build the TIOCSWINSZ.
	*/
	ttyresize(tw.tw, tw.th);

	/*
		Resize the per-row cache to match the new grid. Growing
		default-constructs new empty vectors for added rows;
		shrinking drops the trailing rows' caches.
	*/
	imw_row_ops.resize(new_rows);

	/*
		st.c's tresize only dirties rows when the grid GROWS
		(tclearregion in tresize fires only for mincol < col or
		minrow < row). On a width-only shrink no dirty bit is set,
		so the cached row ops keep their old wider x-coords and
		replay past the new tw.tw. redraw() is the public st.c
		entry point that does tfulldirt + draw — one extra full
		emission this frame, but resize is rare and the explicit
		draw() later in term_draw_canvas walks an empty dirty
		array so it costs only the cursor overlay re-emit.
	*/
	redraw();
}

/*
	Keyboard input dispatch. Mirrors x.c::kpress flow at lines
	1843-1895 adapted for ImGui's polling model. Five stages, in
	priority order:
	  Stage 1   — shortcuts via ImGui::Shortcut (clipboard, NumLock,
	              printer routing — Category A)
	  Stage 2   — kmap (special keys via key[])
	  Stage 2b  — Ctrl+letter -> C0 control bytes (Category B —
	              cross-platform parity for macOS)
	  Stage 2c  — Other Ctrl+C0 chords (Ctrl+Space, Ctrl+[, etc.)
	  Stage 3   — InputQueueCharacters (typed text)

	`consumed_this_frame` is the polling-model analog of x.c's early
	`return` — when a higher-priority stage handles input, Stage 3
	skips to avoid double-dispatch (e.g., Ctrl+Shift+C leaking 'c'
	into InputQueueCharacters on platforms that emit control-code
	chars for Ctrl+letter combos).

	Caller must have already verified term_focused && !MODE_KBDLOCK.
*/
static void
imw_dispatch_keyboard(void)
{
	ImGuiIO &io = ImGui::GetIO();
	bool consumed_this_frame = false;

	/*
		Stage 1: application shortcuts. Mirrors x.c::kpress lines
		1865-1870. ImGui::Shortcut with RouteFocused only fires when
		the focused widget owns the route — without that flag the
		chord would dispatch globally. ImGui handles internal chord
		consumption automatically; we still set consumed_this_frame
		as belt-and-suspenders against char-queue leakage on platforms
		that emit control-code chars for Ctrl+letter combos.
	*/
	for (Shortcut *s = shortcuts; s < shortcuts + LEN(shortcuts); s++) {
		if (ImGui::Shortcut(s->chord, ImGuiInputFlags_RouteFocused)) {
			s->func(&s->arg);
			consumed_this_frame = true;
			break;  /*  one shortcut per frame  */
		}
	}

	/*
		Stage 2: kmap — special keys via the key[] table. Mirrors
		x.c::kmap (lines 1806-1840). For each entry: key pressed
		(repeat=true so held arrows/backspace auto-repeat), mods
		match (with ignoremod stripping), appkey/appcursor three-
		valued logic against MODE_APPKEYPAD / MODE_APPCURSOR. First
		match wins; entry order in config-imgui.def.h matters
		(Shift+Tab listed before any-mod Tab, etc.). The mappedkeys[]
		fast-skip from x.c is dropped — the key[] table is small
		enough that iteration is cheap.
	*/
	if (!consumed_this_frame) {
		for (Key *kp = key; kp < key + LEN(key); kp++) {
			if (!ImGui::IsKeyPressed(kp->k, /*  repeat  */true))
				continue;
			if (!imw_match_mods(kp->mods, io.KeyMods))
				continue;

			/*
				appkey: 0 indifferent, +1 only-when-APPKEYPAD,
				-1 only-when-NOT-APPKEYPAD, +2 numlock-paired.
			*/
			if ((tw.mode & MODE_APPKEYPAD)
			    ? kp->appkey < 0
			    : kp->appkey > 0)
				continue;
			if ((tw.mode & MODE_NUMLOCK) && kp->appkey == 2)
				continue;

			/*  appcursor: same three-valued logic vs MODE_APPCURSOR.  */
			if ((tw.mode & MODE_APPCURSOR)
			    ? kp->appcursor < 0
			    : kp->appcursor > 0)
				continue;

			ttywrite(kp->s, strlen(kp->s), 1);
			consumed_this_frame = true;
			break;
		}
	}

	/*
		Stage 2b: Ctrl+letter -> C0 control bytes.

		Why this stage exists: on macOS, ImGui's char queue doesn't
		receive entries for Ctrl+letter combinations — the OS reserves
		them for app shortcuts and never delivers them as typed
		characters. On Linux/X11, XLookupString translates Ctrl+C ->
		0x03 directly into the composed-text buffer, so x.c gets it
		through Stage 3 for free. To match cross-platform, we
		explicitly translate here.

		Modifier strictness:
		  Ctrl+letter           -> C0 byte (THIS stage, -> PTY)
		  Ctrl+Shift+letter     -> application shortcut (Stage 1)
		  Ctrl+Alt+letter       -> not handled (defer)
		The strict !Shift check ensures Ctrl+Shift+C falls through to
		Stage 1 (shortcuts) for clipcopy.

		Letter range only. The non-letter Ctrl+C0 chords that don't
		require Shift (Ctrl+Space, Ctrl+[, Ctrl+\, Ctrl+]) are handled
		separately in Stage 2c below. The Shift-required ones
		(Ctrl+@ via Shift+2, Ctrl+^ via Shift+6, Ctrl+_ via Shift+-)
		are skipped — they'd need keyboard-layout-aware handling and
		are virtually never typed in practice.

		Conflicts with kmap: Ctrl+I (Tab=0x09), Ctrl+J (LF=0x0A),
		Ctrl+M (CR=0x0D), Ctrl+H (BS=0x08) are already in key[]
		(Tab/Enter/Backspace) and Stage 2 runs first, setting
		consumed_this_frame, so this stage skips them. No
		double-dispatch.

		repeat=true: held Ctrl+C keeps sending SIGINTs (matches
		Linux's auto-repeat -> repeated XLookupString -> repeated 0x03
		stream).
	*/
	static_assert(ImGuiKey_Z - ImGuiKey_A == 25,
	              "ImGuiKey_A..ImGuiKey_Z must be contiguous");
	if (!consumed_this_frame
	    && io.KeyCtrl && !io.KeyShift
	    && !io.KeyAlt && !io.KeySuper) {
		for (int k = ImGuiKey_A; k <= ImGuiKey_Z; k++) {
			if (ImGui::IsKeyPressed((ImGuiKey)k, /*  repeat  */true)) {
				char byte = (char)(k - ImGuiKey_A + 1);
				ttywrite(&byte, 1, 1);
				consumed_this_frame = true;
				break;  /*  one Ctrl+letter per frame  */
			}
		}
	}

	/*
		Stage 2c: Other Ctrl+C0 mappings beyond Ctrl+letter. Linux/X11
		gets these via XLookupString; macOS doesn't, so we translate
		explicitly. Same modifier strictness as Stage 2b (Ctrl held,
		no Shift/Alt/Super).

		Subset chosen for portability — only chords whose key has no
		Shift in its base form on a US layout. Ctrl+@ (Shift+2),
		Ctrl+^ (Shift+6), Ctrl+_ (Shift+-) are skipped: they'd need
		layout-aware handling and are virtually never typed in
		practice. Ctrl+Space → NUL covers the same byte as Ctrl+@
		anyway (it's the common emacs `set-mark-command` binding).

		Mappings:
		  Ctrl+Space  -> 0x00 (NUL)
		  Ctrl+[      -> 0x1B (ESC — alternate form)
		  Ctrl+\      -> 0x1C (FS — terminal SIGQUIT in some configs)
		  Ctrl+]      -> 0x1D (GS — telnet/screen escape char)
	*/
	static const struct {
		ImGuiKey key;
		char     byte;
	} ctrl_c0_map[] = {
		{ ImGuiKey_Space,        0x00 },
		{ ImGuiKey_LeftBracket,  0x1B },
		{ ImGuiKey_Backslash,    0x1C },
		{ ImGuiKey_RightBracket, 0x1D },
	};
	if (!consumed_this_frame
	    && io.KeyCtrl && !io.KeyShift
	    && !io.KeyAlt && !io.KeySuper) {
		for (size_t i = 0;
		     i < sizeof(ctrl_c0_map) / sizeof(ctrl_c0_map[0]); i++) {
			if (ImGui::IsKeyPressed(ctrl_c0_map[i].key,
			                        /*  repeat  */true)) {
				ttywrite(&ctrl_c0_map[i].byte, 1, 1);
				consumed_this_frame = true;
				break;  /*  one chord per frame  */
			}
		}
	}

	/*
		Stage 3: text input — drain the per-frame char queue. Mirrors
		x.c lines 1878-1894 (composed-string path after shortcut +
		kmap fallthroughs). MODE_8BIT handling for Alt+ASCII at lines
		1881-1891.
	*/
	if (!consumed_this_frame) {
		for (int i = 0; i < io.InputQueueCharacters.Size; i++) {
			unsigned int c = io.InputQueueCharacters[i];
			char buf[8];
			int n;

			if (io.KeyAlt && c < 0x80) {
				/*
					Alt+ASCII: high-bit byte if MODE_8BIT,
					ESC-prefix otherwise.
				*/
				if (tw.mode & MODE_8BIT) {
					n = (int)utf8encode((Rune)(c | 0x80), buf);
				} else {
					buf[0] = '\033';
					buf[1] = (char)c;
					n = 2;
				}
			} else {
				n = (int)utf8encode((Rune)c, buf);
			}
			ttywrite(buf, (size_t)n, 1);  /*  may_echo=1: typed  */
		}
	}
}

/*
	Mouse reporting path: encode press/release/wheel/motion as protocol
	bytes and write them to the PTY. Caller has already verified that
	MODE_MOUSE is active and Shift is not held; local selection /
	mshortcuts do not run on this path.

	X10 mode skips release entirely and skips modifier bits. VT200
	(MOUSEBTN) does full press+release with modifier bits set. SGR vs
	default 6-byte format choice happens inside imw_emit_mouse based on
	MODE_MOUSESGR. Wheel is press-only (release suppressed under
	MOUSEBTN per x.c:397).
*/
static void
imw_dispatch_mouse_report(void)
{
	ImGuiIO &io = ImGui::GetIO();

	ImVec2 mpos = ImGui::GetMousePos();
	int col, row;
	imw_pixel_to_cell(mpos, &col, &row);

	int mods = 0;
	if (!(tw.mode & MODE_MOUSEX10)) {
		mods = (io.KeyShift ? 4  : 0)
		     + (io.KeyAlt   ? 8  : 0)
		     + (io.KeyCtrl  ? 16 : 0);
	}

	/*
		ImGui mouse-button order is L/R/M (0/1/2); X11 protocol order
		is L/M/R (encoded as 0/1/2). Swap right ↔ middle.
	*/
	static const int btn_to_code[3] = { 0, 2, 1 };

	/*
		Local emit wrapper. Calls the format-aware encoder, then
		updates the motion-dedupe state so a motion arriving at the
		same cell as the most-recent emit gets suppressed. Mirrors
		x.c lines 403-404 where ox/oy are updated unconditionally
		after every event encode.
	*/
	auto emit = [&](int btn_code, bool release) {
		imw_emit_mouse(btn_code, col, row, mods, release);
		imw_last_motion_x = col;
		imw_last_motion_y = row;
	};

	for (int b = 0; b < 3; b++) {
		if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(b))
			emit(btn_to_code[b], false);
	}
	for (int b = 0; b < 3; b++) {
		if (!ImGui::IsMouseReleased(b))
			continue;
		if (tw.mode & MODE_MOUSEX10)  /*  X10: press only  */
			continue;
		emit(btn_to_code[b], true);
	}

	/*
		Wheel: code 64 = up, 65 = down. Press-only (x.c:397 suppresses
		wheel release under MOUSEBTN). Hover-gated so scrolling outside
		the canvas doesn't poke the app.
	*/
	if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f) {
		int code = io.MouseWheel > 0 ? 64 : 65;
		emit(code, false);
	}

	/*
		Motion events.

		MODE_MOUSEMOTION: emit motion only while a button is held;
		  encoded as lowest-priority held button (L > M > R matching
		  x.c::mousereport's iteration over X11 buttons 1..3) + 32
		  (motion flag).

		MODE_MOUSEMANY: also emit when no button is held; button code
		  = 3, the protocol's idle-motion sentinel (x.c line 408 maps
		  the loop's btn=12 fall-through to `code += 3`).

		Dedupe by (x, y) — skip if the cursor cell hasn't changed since
		  the most-recent emit. Matches x.c lines 374-376. Without
		  this, MOUSEMANY at 60Hz floods the PTY; MOUSEMOTION re-emits
		  the press cell as motion on every frame the button stays
		  held.

		Both flags can be set simultaneously; MOUSEMANY supersedes
		MOUSEMOTION at the no-button-held branch.
	*/
	if ((tw.mode & MODE_MOUSEMOTION) || (tw.mode & MODE_MOUSEMANY)) {
		int btn_code = -1;

		/*  ImGui indices in protocol-priority order (L/M/R).  */
		static const int priority[3] = { 0, 2, 1 };
		for (int i = 0; i < 3; i++) {
			int b = priority[i];
			if (ImGui::IsMouseDown(b)) {
				btn_code = btn_to_code[b];
				break;
			}
		}
		/*
			No button held -> MOUSEMANY uses the idle sentinel;
			MOUSEMOTION skips entirely.
		*/
		if (btn_code < 0 && (tw.mode & MODE_MOUSEMANY))
			btn_code = 3;

		if (btn_code >= 0
		    && (col != imw_last_motion_x
		        || row != imw_last_motion_y)) {
			emit(btn_code + 32, false);
		}
	}
}

/*
	Local selection drag — click+drag+release on Left button, with
	double/triple-click word/line snap.

	NOT focus-gated: ImGui::InvisibleButton already grabs focus on
	click, so a click on an unfocused canvas both focuses and starts a
	selection (xterm/Terminal.app convention). Mirrors x.c::bpress's
	selstart on Button1 (line 506) and x.c::bmotion's selextend on
	motion (line 588).

	State machine via `imw_selecting`:
	  click   -> selstart, set imw_selecting
	  drag    -> selextend(done=0)        (only while imw_selecting)
	  release -> selextend(done=1), clear imw_selecting
	Tracking the flag ourselves means a drag that leaves the canvas
	still extends (matches x.c, where pointer-grab on press keeps
	motion events flowing until release). IsItemHovered() is only
	checked at click-time so an off-canvas press doesn't start a
	selection.

	selmasks[] picks SEL_REGULAR vs SEL_RECTANGULAR by the held modifier
	(Alt for rectangular, by default). Index 0 is unused (SEL_IDLE);
	start at 1. The chosen type is latched at click and reused for
	drag/release so flipping Alt mid-drag doesn't morph an in-progress
	selection.
*/
static void
imw_dispatch_mouse_select(void)
{
	ImGuiIO &io = ImGui::GetIO();
	static int sel_type = SEL_REGULAR;

	if (ImGui::IsItemHovered()
	    && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
		sel_type = SEL_REGULAR;
		for (int t = 1; t < (int)LEN(selmasks); t++) {
			if (imw_match_mods(selmasks[t], io.KeyMods)) {
				sel_type = t;
				break;
			}
		}

		/*
			Snap: double-click -> word, triple+ -> line.
			MouseClickedCount is only meaningful inside the
			IsMouseClicked branch (stale otherwise). >= 3 (not == 3)
			so quad-click and beyond stay on SNAP_LINE rather than
			resetting — matches xterm/Terminal.app.

			Snap is orthogonal to sel_type: a double-click with Alt
			held is a rectangular word-snapped selection. st.c
			latches the snap into sel state at selstart, so a
			subsequent drag extends by words/lines through the same
			selextend(SEL_REGULAR/RECTANGULAR) call — no special
			handling needed in the drag branch.
		*/
		int snap = 0;
		int n = io.MouseClickedCount[ImGuiMouseButton_Left];
		if      (n == 2) snap = SNAP_WORD;
		else if (n >= 3) snap = SNAP_LINE;

		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		imw_pixel_to_cell(mpos, &col, &row);
		selstart(col, row, snap);
		imw_selecting = true;
	}
	if (imw_selecting
	    && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		imw_pixel_to_cell(mpos, &col, &row);
		selextend(col, row, sel_type, 0);
	}
	if (imw_selecting
	    && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		imw_pixel_to_cell(mpos, &col, &row);
		selextend(col, row, sel_type, 1);
		imw_selecting = false;
	}
}

/*
	Mouse-shortcut table walk. Mirrors x.c::mouseaction (line 449) for
	explicit button bindings, and folds the scroll-wheel axis into the
	same dispatch by synthesizing IMW_MB_WHEELUP/_DOWN button events.
	Same shape as the keyboard shortcuts[] walk: iterate the table,
	first match with correct (button, mods, release) wins.

	Mod match policy mirrors x.c — accept either an exact match OR an
	exact match after stripping forcemousemod from state. This lets a
	Shift-held click match an IMW_MOD_ANY entry (the "Shift forces
	terminal" semantic).

	Hover-gated for the press edge and wheel: clicks/scrolls outside
	the canvas should not fire terminal mshortcuts. The release edge is
	NOT hover-gated — pressing on the canvas and dragging off should
	still fire release-bound entries (matches ImGui's pointer-grab
	semantics on the InvisibleButton).

	Conflict with the selection-drag block: today no Left-button
	mshortcut exists, so the press-walk returns no match for Left and
	selection proceeds normally. If a Left-button mshortcut is ever
	added, the selstart in imw_dispatch_mouse_select will need a
	suppress flag based on a Left match here.
*/
static void
imw_dispatch_mouse_mshortcuts(void)
{
	ImGuiIO &io = ImGui::GetIO();
	int state = io.KeyMods;

	auto walk = [&](int button, unsigned int release) {
		for (MouseShortcut *ms = mshortcuts;
		     ms < mshortcuts + LEN(mshortcuts); ms++) {
			if (ms->release != release)   continue;
			if (ms->button  != button)    continue;
			if (!imw_match_mods(ms->mods, state)
			    && !imw_match_mods(ms->mods,
			                       state & ~forcemousemod))
				continue;
			ms->func(&ms->arg);
			return true;
		}
		return false;
	};

	/*
		Real buttons — Left/Right/Middle/X1/X2. ImGui validates the
		button index against IM_ARRAYSIZE(io.MouseDown)==5.
	*/
	for (int b = 0; b < 5; b++) {
		if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(b))
			walk(b, 0);
		if (ImGui::IsMouseReleased(b))
			walk(b, 1);
	}

	/*
		Wheel — io.MouseWheel positive = up, negative = down.
		Press-only (wheel has no release edge). Hover-gated so
		scrolling outside the canvas doesn't fire terminal actions.
		Only one edge per frame even if wheel delta is multi-tick —
		mshortcut funcs are typically idempotent (selpaste, scrollback
		step) and per-tick fan-out is a future refinement.
	*/
	if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
		walk(io.MouseWheel > 0 ? IMW_MB_WHEELUP
		                       : IMW_MB_WHEELDOWN, 0);
}

/*
	Mouse dispatcher. Splits into two mutually exclusive paths at the
	press level:

	  report_mouse  (MODE_MOUSE active AND Shift NOT held)
	    -> app owns the mouse: encode events and ttywrite protocol
	      bytes; local selection / mshortcuts do not run.
	  !report_mouse (the common case, plus Shift escape hatch)
	    -> user owns the mouse: selection, mshortcuts, and
	      wheel-as-button run as before.

	Shift is hardcoded as the bypass modifier (matches upstream st;
	the `forcemousemod` config knob is vestigial). Hold Shift inside
	vim/tmux to drop back to local selection.
*/
static void
imw_dispatch_mouse(void)
{
	ImGuiIO &io = ImGui::GetIO();
	bool report_mouse = (tw.mode & MODE_MOUSE) && !io.KeyShift;

	if (report_mouse) {
		imw_dispatch_mouse_report();
	} else {
		imw_dispatch_mouse_select();
		imw_dispatch_mouse_mshortcuts();
	}
}

/*  Public widget API  */

void
term_init(int cols, int rows, char **argv)
{
	/*
		Provisional cell metrics — replaced after first frame's atlas bake
		by imw_finalize_metrics(). The provisional values let tnew() proceed
		with reasonable initial buffer sizes.
	*/
	tw.cw = 8;
	tw.ch = 16;
	tw.tw = cols * tw.cw;
	tw.th = rows * tw.ch;
	tw.w  = tw.tw;
	tw.h  = tw.th;
	tw.mode   = MODE_VISIBLE | MODE_FOCUSED | MODE_NUMLOCK;
	tw.cursor = 2; /*  steady block (config.def.h default)  */

	imw_load_fonts();   /*  Fonts queued in atlas, baked on first frame  */
	imw_loadcols();
	imw_row_ops.resize(rows);  /*  per-row glyph cache; grown by imw_handle_resize  */
	tnew(cols, rows);
	selinit();

	/*
		Spawn child shell via st.c's PTY plumbing. NULL args -> defaults.
		Returned fd is the master side; st.c stashes
		its own copy as `cmdfd` for ttyread/ttywrite. We keep a copy for
		the per-frame select() poll.
	*/
	imw_cmdfd = ttynew(NULL, NULL, NULL, argv);
	if (imw_cmdfd < 0)
		die("imgui_win: ttynew failed\n");
}

/*
	term_draw_canvas — render the terminal into the *current* ImGui
	window. The caller owns Begin/End and any window styling. Focus is
	the standard ImGui IsItemFocused() on the InvisibleButton submitted
	below: clicks on the canvas focus it, clicks elsewhere unfocus it,
	all the usual rules. To force focus without a real click (initial
	focus on a pinned native window, test harnesses), call
	ImGui::SetKeyboardFocusHere() in the same Begin/End immediately
	before calling this — the InvisibleButton is the first ImGui item
	submitted here, so it receives the targeted focus.

	If you just want a self-contained floating ImGui window with a
	terminal in it, call term_draw_widget() instead.
*/
void
term_draw_canvas(void)
{
	/*  First frame after atlas is baked — derive real cell metrics.  */
	if (!dc.metrics_derived)
		imw_finalize_metrics();

	ImVec2 avail = ImGui::GetContentRegionAvail();
	imw_handle_resize(avail);

	/*
		Reserve canvas. InvisibleButton handles click-to-focus AND must
		remain the FIRST ImGui item submitted here — SetKeyboardFocusHere
		called by the host before this function targets the next item,
		so anything inserted ahead of this would silently steal focus.

		EnableNav is required: InvisibleButton sets ImGuiItemFlags_NoNav
		by default, which excludes it from keyboard nav and makes
		SetKeyboardFocusHere() a no-op against it. Without the flag,
		clicks still focus (different code path) but programmatic
		focus from hosts/tests can't.
	*/
	ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##term_canvas", avail,
	                       ImGuiButtonFlags_MouseButtonLeft  |
	                       ImGuiButtonFlags_MouseButtonRight |
	                       ImGuiButtonFlags_MouseButtonMiddle |
	                       ImGuiButtonFlags_EnableNav);
	bool focused = ImGui::IsItemFocused();

	/*
		Focus drives MODE_FOCUSED (cursor style), the keyboard-dispatch
		gate below, DECSET 1004 \033[I / \033[O emission edge-triggered
		against imw_was_focused, and SetNextFrameWantCaptureKeyboard so
		the platform backend's keyDown handler consumes the NSEvent
		instead of bubbling it up to NSApp's "no responder" beep.
	*/
	if (focused) {
		ImGui::SetNextFrameWantCaptureKeyboard(true);
		tw.mode |=  MODE_FOCUSED;
	} else {
		tw.mode &= ~MODE_FOCUSED;
	}
	if ((tw.mode & MODE_FOCUS) && focused != imw_was_focused)
		ttywrite(focused ? "\033[I" : "\033[O", 3, 0);
	imw_was_focused = focused;

	/*
		Set the per-frame render context for xclear / xdrawglyphfontspecs.
		Two fields, by design — see ImwRenderCtx comment.
	*/
	imw_ctx.canvas_pos = canvas_pos;
	imw_ctx.dl         = ImGui::GetWindowDrawList();

	/*
		Background fill for the entire canvas. Direct draw (not
		cached): the canvas-wide fill isn't tied to any row, runs
		every frame, and provides the backdrop for areas that the
		row caches don't cover (mainly during a resize where new
		rows haven't been recorded yet).

		Under MODE_REVERSE the canvas inverts: default-bg slots
		become default-fg, mirroring x.c's xclear (line 856)
		which picks `defaultfg` vs `defaultbg` by IS_SET(MODE_REVERSE).
	*/
	{
		ImU32 bg_col = colors[(tw.mode & MODE_REVERSE)
		                       ? defaultfg : defaultbg];
		ImVec2 bg_p1(canvas_pos.x + avail.x,
		             canvas_pos.y + avail.y);
		imw_ctx.dl->AddRectFilled(canvas_pos, bg_p1, bg_col);
	}

	/*  Drain whatever the shell has emitted since last frame.  */
	imw_pump_pty();

	/*
		Tick the blink timer (toggles MODE_BLINK every blinktimeout ms
		when any cell has ATTR_BLINK).
	*/
	imw_tick_blink();

	/*  Keyboard input dispatch (focus-gated + KBDLOCK-gated).  */
	if (focused && !(tw.mode & MODE_KBDLOCK))
		imw_dispatch_keyboard();

	/*  Mouse input dispatch — selection vs reporting, by mode.  */
	imw_dispatch_mouse();

	/*
		Run a draw cycle: xdrawline writes per-row ops into the
		cache, xdrawcursor writes the cursor into the per-frame
		overlay. Phase C: use draw() to lean on st.c's dirty-tracking
		to skip clean rows. Only rows that changed since the last
		frame (or the cursor row) will re-record.
	*/
	if (dc.metrics_derived)
		draw();

	/*
		Replay all row caches in row order, then the cursor
		overlay on top. canvas_pos is added at replay time so a
		window drag (which changes canvas_pos but doesn't dirty
		any rows) doesn't invalidate the cache.
	*/
	for (size_t y = 0; y < imw_row_ops.size(); y++)
		imw_replay_ops(imw_row_ops[y], canvas_pos, imw_ctx.dl);
	imw_replay_ops(imw_overlay_ops, canvas_pos, imw_ctx.dl);
}

/*
	term_draw_widget — convenience wrapper. A self-contained floating
	ImGui window with the terminal canvas inside, click-to-focus model.
	Used by simple shells (e.g. examples/main_example_glfw_gl.cpp,
	examples/example_mac_metal.mm) that don't want to manage their own
	ImGui window.

	Shells that need control of the windowing (native-window pinning,
	custom flags / styling, multi-pane layouts, etc.) should call
	term_draw_canvas directly inside their own Begin/End instead.
*/
void
term_draw_widget(void)
{
	/*  Title with persistent ID.  */
	char label[300];
	snprintf(label, sizeof label, "%s###st_term_widget", term_title);

	/*
		First-use default size. ImGui's untouched default is a tiny
		debug-overlay-sized window, which makes the terminal start
		barely usable. ImGuiCond_FirstUseEver only applies when there's
		no saved state in imgui.ini for this window, so once the user
		resizes (or imgui.ini is present) this is a no-op.
	*/
	ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);

	if (ImGui::Begin(label))
		term_draw_canvas();
	ImGui::End();
}

void
term_shutdown(void)
{
	/*
		Send SIGHUP to the child shell so it cleans up; closes cmdfd
		inside st.c. Mirrors x.c via cmessage's WM_DELETE handler path.
	*/
	if (imw_cmdfd >= 0) {
		ttyhangup();
		imw_cmdfd = -1;
	}
	/*
		Note: ImGui fonts and the rgb_db live to ImGui-context-destruction
		time; the host owns that, not us. We don't free `dc.col[]` because
		`colors[]` is a static array, not heap.
	*/
}

/*
	term_dump_json — verbatim serialization of the adapter's draw-op cache.
	No interpretation: every entry in imw_row_ops[] and imw_overlay_ops is
	emitted as-is. Forced canonicalizations (pointer/u32 → portable string)
	only:
	  - col (ImU32) → "#rrggbb" (raw int isn't portable across runs)
	  - font (ImFont*) → 4-way slot match against dc.font/bfont/ifont/ibfont
	    or "other" (pointer addresses aren't stable across runs)
	  - text bytes → JSON-escaped string of the literal op.bytes[0..len]
	Adapter-state scalars (cw/ch/tw/th, mode, cursor shape, title) come
	straight from `tw` and `term_title`. Reads no st.c state.
*/
static const char *
imw_dump_font_name(ImFont *f)
{
	if (f == dc.font.match)    return "regular";
	if (f == dc.bfont.match)   return "bold";
	if (f == dc.ifont.match)   return "italic";
	if (f == dc.ibfont.match)  return "bold_italic";
	return "other";
}

static void
imw_dump_color(FILE *out, ImU32 c)
{
	int r = (int)((c >>  0) & 0xff);
	int g = (int)((c >>  8) & 0xff);
	int b = (int)((c >> 16) & 0xff);
	fprintf(out, "\"#%02x%02x%02x\"", r, g, b);
}

static void
imw_dump_jstr(FILE *out, const char *s, int len)
{
	fputc('"', out);
	for (int i = 0; i < len; i++) {
		unsigned char c = (unsigned char)s[i];
		switch (c) {
		case '"':  fputs("\\\"", out); break;
		case '\\': fputs("\\\\", out); break;
		case '\b': fputs("\\b",  out); break;
		case '\f': fputs("\\f",  out); break;
		case '\n': fputs("\\n",  out); break;
		case '\r': fputs("\\r",  out); break;
		case '\t': fputs("\\t",  out); break;
		default:
			if (c < 0x20) fprintf(out, "\\u%04x", c);
			else          fputc(c, out);
		}
	}
	fputc('"', out);
}

static void
imw_dump_op(FILE *out, const DrawOp &op)
{
	switch (op.kind) {
	case DrawOp::RECT:
		fprintf(out, "{\"kind\":\"RECT\",\"p0\":[%g,%g],\"p1\":[%g,%g],\"col\":",
		    op.p0.x, op.p0.y, op.p1.x, op.p1.y);
		imw_dump_color(out, op.col);
		fputc('}', out);
		break;
	case DrawOp::TEXT:
		fprintf(out, "{\"kind\":\"TEXT\",\"p0\":[%g,%g],\"col\":",
		    op.p0.x, op.p0.y);
		imw_dump_color(out, op.col);
		fprintf(out, ",\"font\":\"%s\",\"text\":", imw_dump_font_name(op.font));
		imw_dump_jstr(out, (const char *)op.bytes, op.len);
		fputc('}', out);
		break;
	case DrawOp::PUSH_CLIP:
		fprintf(out, "{\"kind\":\"PUSH_CLIP\",\"p0\":[%g,%g],\"p1\":[%g,%g]}",
		    op.p0.x, op.p0.y, op.p1.x, op.p1.y);
		break;
	case DrawOp::POP_CLIP:
		fputs("{\"kind\":\"POP_CLIP\"}", out);
		break;
	}
}

static void
imw_dump_oplist(FILE *out, const std::vector<DrawOp> &ops)
{
	for (size_t i = 0; i < ops.size(); i++) {
		if (i) fputc(',', out);
		imw_dump_op(out, ops[i]);
	}
}

void
term_dump_json(FILE *out)
{
	fprintf(out, "{\n");
	fprintf(out, "  \"cw\": %d,\n",            tw.cw);
	fprintf(out, "  \"ch\": %d,\n",            tw.ch);
	fprintf(out, "  \"tw\": %d,\n",            tw.tw);
	fprintf(out, "  \"th\": %d,\n",            tw.th);
	/* Mask out MODE_FOCUS for cross-platform test stability.
	   ConPTY unconditionally sends \033[?1004h which sets this
	   bit; raw POSIX PTYs don't. The bit is ConPTY infrastructure,
	   not application state, so stripping it produces baselines
	   that match across platforms. */
	int dump_mode = tw.mode;
#ifdef _WIN32
	dump_mode &= ~MODE_FOCUS;
#endif
	fprintf(out, "  \"mode\": %d,\n",          dump_mode);
	fprintf(out, "  \"cursor_shape\": %d,\n",  tw.cursor);
	/* Use the default title for dump stability. ConPTY and some
	   shells send OSC title-set sequences (exe path, PS1 \w, etc.)
	   before the script runs — these are environment-dependent and
	   would break cross-platform test baselines. */
	const char *dump_title = "Terminal";
	fprintf(out, "  \"title\": ");
	imw_dump_jstr(out, dump_title, (int)strlen(dump_title));
	fprintf(out, ",\n");

	int rows = (int)imw_row_ops.size();
	fprintf(out, "  \"rows\": [");
	for (int r = 0; r < rows; r++) {
		fputs(r ? ",\n    " : "\n    ", out);
		fprintf(out, "{\"row\":%d,\"ops\":[", r);
		imw_dump_oplist(out, imw_row_ops[r]);
		fputc(']', out);
		fputc('}', out);
	}
	fprintf(out, "\n  ],\n");

	fprintf(out, "  \"overlay\": [");
	imw_dump_oplist(out, imw_overlay_ops);
	fprintf(out, "]\n");
	fprintf(out, "}\n");
}



/*
	win.h contract surface — 16 thunks that satisfy the contract st.c
	expects. Each forwards to its imw_* implementation in one line.
	The split lets implementations have descriptive names matching
	what the file actually is (an ImGui adapter), while the linker-
	visible contract names stay locked to upstream's convention because
	st.c calls them by name. Compiler inlines the thunks at any
	optimization level.
*/
extern "C" {

void xbell(void)
	{ imw_bell(); }

void xclipcopy(void)
	{ imw_clipcopy(); }

void xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
	{ imw_drawcursor(cx, cy, g, ox, oy, og); }

void xdrawline(Line line, int x1, int y, int x2)
	{ imw_drawline(line, x1, y, x2); }

void xfinishdraw(void)
	{ imw_finishdraw(); }

void xloadcols(void)
	{ imw_loadcols(); }

int xsetcolorname(int x, const char *name)
	{ return imw_setcolorname(x, name); }

int xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
	{ return imw_getcolor(x, r, g, b); }

void xseticontitle(char *p)
	{ imw_seticontitle(p); }

void xsettitle(char *p)
	{ imw_settitle(p); }

int xsetcursor(int cursor)
	{ return imw_setcursor(cursor); }

void xsetmode(int set, unsigned int flags)
	{ imw_setmode(set, flags); }

void xsetpointermotion(int set)
	{ imw_setpointermotion(set); }

void xsetsel(char *str)
	{ imw_setsel(str); }

int xstartdraw(void)
	{ return imw_startdraw(); }

void xximspot(int x, int y)
	{ imw_ximspot(x, y); }

} /*  extern "C" — win.h contract surface  */
