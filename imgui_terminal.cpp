/* 	See LICENSE for license details. 

	imgui_terminal.cpp - ImGui-native terminal based on the st terminal emulator (suckless.org/st).
	No platform code, no OpenGL, no Vulkan. Only ImGui APIs.

*/

#include "imgui_terminal.h"

#include "callbacks.h"
#include "imgui_freetype.h"

#include <fontconfig/fontconfig.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iterator>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/select.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

/* ----------------------------------------------------------------------
    Emulator config globals — shared with emulator.c via extern "C".
   ---------------------------------------------------------------------- */

extern "C"
{

	char *utmp = NULL;
	char *scroll = NULL;
	char *stty_args = (char *) "stty raw pass8 nl -echo -iexten -cstopb 38400";
	char *vtiden = (char *) "\033[?6c";
	wchar_t *worddelimiters = (wchar_t *) L" ";
	int allowaltscreen = 1;
	int allowwindowops = 0;
	char *termname = (char *) "st-256color";
	unsigned int tabspaces = 8;
	unsigned int defaultfg = 258;
	unsigned int defaultbg = 259;
	unsigned int defaultcs = 256;
}

/* ----------------------------------------------------------------------
   Emit helpers: write per-frame draw ops into the active emit target
   (per-row cache or overlay).
   ---------------------------------------------------------------------- */

void
Terminal::emit_rect(ImVec2 p0, ImVec2 p1, ImU32 col)
{
	DrawOp op;
	op.kind = DrawOp::RECT;
	op.p0 = p0;
	op.p1 = p1;
	op.col = col;
	op.font = NULL;
	op.len = 0;
	emit_target->push_back(op);
}

void
Terminal::emit_text(ImFont *font, ImVec2 pos, ImU32 col, const char *txt, int n)
{
	DrawOp op;
	op.kind = DrawOp::TEXT;
	op.p0 = pos;
	op.p1 = ImVec2(0, 0);
	op.col = col;
	op.font = font;
	op.len = (uint8_t) ((n > (int) sizeof op.bytes) ? sizeof op.bytes : n);
	memcpy(op.bytes, txt, op.len);
	emit_target->push_back(op);
}

void
Terminal::emit_push_clip(ImVec2 p0, ImVec2 p1)
{
	DrawOp op;
	op.kind = DrawOp::PUSH_CLIP;
	op.p0 = p0;
	op.p1 = p1;
	op.col = 0;
	op.font = NULL;
	op.len = 0;
	emit_target->push_back(op);
}

void
Terminal::emit_pop_clip()
{
	DrawOp op;
	op.kind = DrawOp::POP_CLIP;
	op.p0 = ImVec2(0, 0);
	op.p1 = ImVec2(0, 0);
	op.col = 0;
	op.font = NULL;
	op.len = 0;
	emit_target->push_back(op);
}

// canvas_pos added at replay so dragging doesn't invalidate cache
void
Terminal::replay_ops(const std::vector<DrawOp> &ops, ImVec2 origin, ImDrawList *dl)
{
	for (size_t i = 0; i < ops.size(); i++)
	{
		const DrawOp &op = ops[i];
		ImVec2 p0(origin.x + op.p0.x, origin.y + op.p0.y);
		ImVec2 p1(origin.x + op.p1.x, origin.y + op.p1.y);
		switch (op.kind)
		{
			case DrawOp::RECT:
				if (transparent_bg && op.col == colors[defaultbg])
					break;
				dl->AddRectFilled(p0, p1, op.col);
				break;
			case DrawOp::TEXT:
				dl->AddText(op.font, s_dc.font.pxsize, p0, op.col,
				    (const char *) op.bytes, (const char *) op.bytes + op.len);
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

// ---- Emulator callbacks (on_*) ----

void
Terminal::on_bell()
{
	// not implemented.
}

void
Terminal::on_clipcopy()
{
	char *sel = getsel(&e);
	if (sel)
	{
		ImGui::SetClipboardText(sel);
		free(sel);
	}
}

void
Terminal::on_die(const char *msg)
{
	/* A fatal error confined to this terminal: surface it without taking
	   down the process. The emulator marks e.alive = 0 right after this
	   returns, so is_alive() will report the terminal as dead. */
	fprintf(stderr, "imgui_terminal: %s", msg ? msg : "fatal error\n");
	snprintf(title, sizeof title, "[exited] %s", msg ? msg : "");
	title[sizeof title - 1] = '\0';
}

void
Terminal::on_drawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	(void) ox;
	(void) oy;
	(void) og;
	emit_target = &overlay_ops;

	if (tw.mode & MODE_HIDE)
		return;

	g.mode &= ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_STRUCK | ATTR_WIDE;

	ImU32 drawcol;
	if (tw.mode & MODE_REVERSE)
	{
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (selected(&e, cx, cy))
		{
			drawcol = colors[defaultcs];
			g.fg = s_defaultrcs;
		}
		else
		{
			drawcol = colors[s_defaultrcs];
			g.fg = defaultcs;
		}
	}
	else
	{
		if (selected(&e, cx, cy))
		{
			g.fg = defaultfg;
			g.bg = s_defaultrcs;
		}
		else
		{
			g.fg = defaultbg;
			g.bg = defaultcs;
		}
		drawcol = colors[g.bg];
	}

	int winx = s_borderpx + cx * tw.cw;
	int winy = s_borderpx + cy * tw.ch;

	if (tw.mode & MODE_FOCUSED)
	{
		switch (tw.cursor)
		{
			case 7:
				g.u = 0x2603;
				/*  fallthrough  */
			case 0:
			case 1:
			case 2:
				drawglyph_internal(g, cx, cy);
				break;
			case 3:
			case 4:
				clear_rect(winx, winy + tw.ch - (int) s_cursorthickness,
				    winx + tw.cw, winy + tw.ch, drawcol);
				break;
			case 5:
			case 6:
				clear_rect(winx, winy, winx + (int) s_cursorthickness, winy + tw.ch,
				    drawcol);
				break;
		}
	}
	else
	{
		clear_rect(winx, winy, winx + tw.cw - 1, winy + 1, drawcol);
		clear_rect(winx, winy, winx + 1, winy + tw.ch - 1, drawcol);
		clear_rect(winx + tw.cw - 1, winy, winx + tw.cw, winy + tw.ch - 1, drawcol);
		clear_rect(winx, winy + tw.ch - 1, winx + tw.cw, winy + tw.ch, drawcol);
	}
}

void
Terminal::on_drawline(Line line, int x1, int y, int x2)
{
	if (y >= 0 && y < (int) row_ops.size())
		row_ops[y].clear();
	emit_target = (y >= 0 && y < (int) row_ops.size()) ? &row_ops[y] : NULL;
	if (emit_target == NULL)
		return;

	int numspecs = makeglyphfontspecs(specs_buf, &line[x1], x2 - x1, x1, y);

	ImwGlyphSpec *specs = specs_buf;
	Glyph base = {};
	Glyph neu; /*  `new` is a C++ keyword  */
	int i = 0;
	int ox = 0;

	for (int x = x1; x < x2 && i < numspecs; x++)
	{
		neu = line[x];
		if (neu.mode == ATTR_WDUMMY)
			continue;
		if (selected(&e, x, y))
			neu.mode ^= ATTR_REVERSE;
		if (i > 0 && attrcmp(base, neu))
		{
			drawglyphfontspecs(specs, base, i, ox, y);
			specs += i;
			numspecs -= i;
			i = 0;
		}
		if (i == 0)
		{
			ox = x;
			base = neu;
		}
		i++;
	}
	if (i > 0)
		drawglyphfontspecs(specs, base, i, ox, y);
}

void
Terminal::on_finishdraw()
{
	//  No-op: under widget model, host owns the render flush.
}

void
Terminal::on_loadcols()
{
	load_rgb_db();
	for (int i = 0; i < IMW_COLORS_LEN; i++)
	{
		ImU32 c = 0;
		if (!resolve_color_at(i, NULL, &c))
			die("imgui_terminal: cannot derive color slot %d\n", i);
		colors[i] = c;
	}
}

int
Terminal::on_setcolorname(int x, const char *name)
{
	if (!between(x, 0, IMW_COLORS_LEN - 1))
		return 1;
	ImU32 c;
	if (!resolve_color_at(x, name, &c))
		return 1;
	colors[x] = c;
	return 0;
}

int
Terminal::on_getcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!between(x, 0, IMW_COLORS_LEN - 1))
		return 1;
	ImU32 c = colors[x];
	*r = (c >> IM_COL32_R_SHIFT) & 0xff;
	*g = (c >> IM_COL32_G_SHIFT) & 0xff;
	*b = (c >> IM_COL32_B_SHIFT) & 0xff;
	return 0;
}

void
Terminal::on_seticontitle(char *p)
{
	(void) p;
}

void
Terminal::on_settitle(char *p)
{
	snprintf(title, sizeof title, "%s", p ? p : "Terminal");
}

int
Terminal::on_setcursor(int cursor)
{
	if (!between(cursor, 0, 7))
		return 1;
	tw.cursor = cursor;
	return 0;
}

void
Terminal::on_setmode(int set, unsigned int flags)
{
	int prev = tw.mode;
	tw.mode = modbit(tw.mode, set, flags);
	//  Full redraw on whole-screen-reverse-video flip.
	if ((tw.mode & MODE_REVERSE) != (prev & MODE_REVERSE))
		redraw(&e);

	if ((tw.mode & MODE_MOUSE) != (prev & MODE_MOUSE))
	{
		selecting = false;
		last_motion_x = -1;
		last_motion_y = -1;
	}
}

void
Terminal::on_setpointermotion(int set)
{
	(void) set;
}

void
Terminal::on_setsel(char *str)
{
	if (str)
		ImGui::SetClipboardText(str);
}

int
Terminal::on_startdraw()
{
	overlay_ops.clear();
	return (tw.mode & MODE_VISIBLE) != 0;
}

void
Terminal::on_ximspot(int x, int y)
{
	(void) x;
	(void) y;
}

/* ---- Color parsing: file-scope helpers, no Terminal state. ---- */

static int
imw_hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool
imw_parse_hex_component(const char *s, int n_digits, uint16_t *out)
{
	int v = 0;
	for (int i = 0; i < n_digits; i++)
	{
		int d = imw_hex_digit(s[i]);
		if (d < 0)
			return false;
		v = (v << 4) | d;
	}
	int max = (1 << (n_digits * 4)) - 1;
	*out = (uint16_t) ((unsigned long) v * 0xFFFF / (unsigned long) max);
	return true;
}

static std::string
imw_normalize_name(const char *name)
{
	std::string out;
	out.reserve(strlen(name));
	for (const char *p = name; *p; p++)
	{
		unsigned char c = (unsigned char) *p;
		if (!isspace(c))
			out += (char) tolower(c);
	}
	return out;
}

bool
Terminal::lookup_rgb(const char *name, uint8_t *r, uint8_t *g, uint8_t *b)
{
	std::string canonical = imw_normalize_name(name);
	auto it = std::lower_bound(rgb_db.begin(), rgb_db.end(), canonical,
	    [](const ColorEntry &e, const std::string &s) { return e.canonical < s; });
	if (it == rgb_db.end() || it->canonical != canonical)
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
	DWORD r = GetModuleFileNameA(NULL, out, (DWORD) n);
	if (r == 0 || r >= n)
		return;
	char *sep = strrchr(out, '\\');
	if (!sep)
		sep = strrchr(out, '/');
	if (sep)
		*sep = '\0';
	return;
#elif defined(__APPLE__)
	uint32_t sz = (uint32_t) n;
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

void
Terminal::load_rgb_db()
{
	if (!rgb_db.empty())
		return;

	FILE *f = NULL;
	char dir[1024];
	imw_get_exe_dir(dir, sizeof dir);
	if (dir[0])
	{
		char path[1024];
		snprintf(path, sizeof path, "%s/../rgb.txt", dir);
		f = fopen(path, "r");
		if (!f)
		{
			snprintf(path, sizeof path, "%s/rgb.txt", dir);
			f = fopen(path, "r");
		}
	}
	if (!f)
		f = fopen("rgb.txt", "r");
	if (!f)
		die("imgui_terminal: cannot open rgb.txt: %s\n", strerror(errno));
	char line[256];
	while (fgets(line, sizeof line, f))
	{
		if (line[0] == '!' || line[0] == '#')
			continue;
		int r, g, b, n;
		if (sscanf(line, "%d %d %d %n", &r, &g, &b, &n) != 3)
			continue;
		char *name = line + n;
		size_t len = strlen(name);
		while (len > 0 && (name[len - 1] == '\n' || name[len - 1] == '\r'))
			name[--len] = '\0';
		if (len == 0)
			continue;
		ColorEntry e;
		e.r = (uint8_t) r;
		e.g = (uint8_t) g;
		e.b = (uint8_t) b;
		e.canonical = imw_normalize_name(name);
		rgb_db.push_back(std::move(e));
	}
	fclose(f);
	std::sort(rgb_db.begin(), rgb_db.end(),
	    [](const ColorEntry &a, const ColorEntry &b) { return a.canonical < b.canonical; });
}

bool
Terminal::parse_color(const char *name, uint8_t *r, uint8_t *g, uint8_t *b)
{
	if (!name)
		return false;

	if (name[0] == '#')
	{
		size_t hexlen = strlen(name + 1);
		if (hexlen != 3 && hexlen != 6 && hexlen != 12)
			return false;
		int per = (int) hexlen / 3;
		uint16_t r16, g16, b16;
		if (!imw_parse_hex_component(name + 1 + 0 * per, per, &r16) ||
		    !imw_parse_hex_component(name + 1 + 1 * per, per, &g16) ||
		    !imw_parse_hex_component(name + 1 + 2 * per, per, &b16))
			return false;
		*r = (uint8_t) (r16 >> 8);
		*g = (uint8_t) (g16 >> 8);
		*b = (uint8_t) (b16 >> 8);
		return true;
	}

	if (strncmp(name, "rgb:", 4) == 0)
	{
		const char *s = name + 4;
		const char *slash1 = strchr(s, '/');
		if (!slash1)
			return false;
		const char *slash2 = strchr(slash1 + 1, '/');
		if (!slash2)
			return false;
		int n_r = (int) (slash1 - s);
		int n_g = (int) (slash2 - slash1 - 1);
		int n_b = (int) strlen(slash2 + 1);
		if (n_r < 1 || n_r > 4 || n_g < 1 || n_g > 4 || n_b < 1 || n_b > 4)
			return false;
		uint16_t r16, g16, b16;
		if (!imw_parse_hex_component(s, n_r, &r16) ||
		    !imw_parse_hex_component(slash1 + 1, n_g, &g16) ||
		    !imw_parse_hex_component(slash2 + 1, n_b, &b16))
			return false;
		*r = (uint8_t) (r16 >> 8);
		*g = (uint8_t) (g16 >> 8);
		*b = (uint8_t) (b16 >> 8);
		return true;
	}

	return lookup_rgb(name, r, g, b);
}

static uint8_t
imw_sixd_to_8bit(int x)
{
	if (x == 0)
		return 0;
	return (uint8_t) ((0x3737 + 0x2828 * x) >> 8);
}

bool
Terminal::resolve_color_at(int i, const char *name, ImU32 *out)
{
	uint8_t r = 0, g = 0, b = 0;

	if (!name)
	{
		if (between(i, 16, 255))
		{
			if (i < 6 * 6 * 6 + 16)
			{
				r = imw_sixd_to_8bit(((i - 16) / 36) % 6);
				g = imw_sixd_to_8bit(((i - 16) / 6) % 6);
				b = imw_sixd_to_8bit(((i - 16) / 1) % 6);
			}
			else
			{
				int gray16 = 0x0808 + 0x0a0a * (i - (6 * 6 * 6 + 16));
				r = g = b = (uint8_t) (gray16 >> 8);
			}
			*out = IM_COL32(r, g, b, 255);
			return true;
		}
		if (i >= 0 && i < 16)
			name = s_colorname_low[i];
		else if (i >= 256 && i < 260)
			name = s_colorname_high[i - 256];
		else
			return false;
	}

	if (!parse_color(name, &r, &g, &b))
		return false;
	*out = IM_COL32(r, g, b, 255);
	return true;
}

/* ----------------------------------------------------------------------
   Glyph rendering.
   ---------------------------------------------------------------------- */

void
Terminal::clear_rect(int x1, int y1, int x2, int y2, ImU32 col)
{
	emit_rect(ImVec2((float) x1, (float) y1), ImVec2((float) x2, (float) y2), col);
}

ImU32
Terminal::resolve_glyph_color(uint32_t c)
{
	if (is_truecol(c))
		return IM_COL32((c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff, 0xff);
	return colors[c];
}

void
Terminal::tick_blink()
{
	double now_ms = ImGui::GetTime() * 1000.0;
	if (now_ms - blink_last_toggle_ms < (double) s_blinktimeout)
		return;
	if (tattrset(&e, ATTR_BLINK))
	{
		tw.mode ^= MODE_BLINK;
		tsetdirtattr(&e, ATTR_BLINK);
	}
	else
	{
		tw.mode |= MODE_BLINK;
	}
	blink_last_toggle_ms = now_ms;
}

bool
Terminal::imw_match_mods(ImGuiKeyChord wanted, int got)
{
	if (wanted == IMW_MOD_ANY)
		return true;
	return wanted == (got & ~s_ignoremod);
}

void
Terminal::pixel_to_cell(ImVec2 p, int *col, int *row)
{
	int x = (int) (p.x - canvas_pos.x) - s_borderpx;
	int y = (int) (p.y - canvas_pos.y) - s_borderpx;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x > tw.tw - 1)
		x = tw.tw - 1;
	if (y > tw.th - 1)
		y = tw.th - 1;
	*col = x / tw.cw;
	*row = y / tw.ch;
}

void
Terminal::emit_mouse_default(int btn_code, int col, int row, int mods, bool release)
{
	if (release)
		btn_code = 3;
	int code = btn_code + mods;

	int cx = col + 1, cy = row + 1;
	if (cx > 223)
		cx = 223;
	if (cy > 223)
		cy = 223;
	if (cx < 1)
		cx = 1;
	if (cy < 1)
		cy = 1;

	char buf[6];
	buf[0] = '\033';
	buf[1] = '[';
	buf[2] = 'M';
	buf[3] = (char) (32 + code);
	buf[4] = (char) (32 + cx);
	buf[5] = (char) (32 + cy);
	ttywrite(&e, buf, 6, 0);
}

void
Terminal::emit_mouse_sgr(int btn_code, int col, int row, int mods, bool release)
{
	int code = btn_code + mods;
	int cx = col + 1, cy = row + 1;
	if (cx < 1)
		cx = 1;
	if (cy < 1)
		cy = 1;

	char buf[32];
	int n = snprintf(buf, sizeof buf, "\033[<%d;%d;%d%c", code, cx, cy, release ? 'm' : 'M');
	if (n > 0 && n < (int) sizeof buf)
		ttywrite(&e, buf, (size_t) n, 0);
}

void
Terminal::emit_mouse(int btn_code, int col, int row, int mods, bool release)
{
	if (tw.mode & MODE_MOUSESGR)
		emit_mouse_sgr(btn_code, col, row, mods, release);
	else
		emit_mouse_default(btn_code, col, row, mods, release);
}

void
Terminal::pump_pty()
{
	if (e.cmdfd < 0)
		return;

	const double drain_budget_s = 0.005;
	const double deadline = ImGui::GetTime() + drain_budget_s;
#ifdef _WIN32
	for (;;)
	{
		DWORD avail = 0;
		if (!PeekNamedPipe(e.w32_pipe_in, NULL, 0, NULL, &avail, NULL))
			break;
		if (avail == 0)
			break;
		::ttyread(&e);
		if (ImGui::GetTime() >= deadline)
			break;
	}
#else
	fd_set rfds;
	struct timeval tv = {0, 0};
	for (;;)
	{
		FD_ZERO(&rfds);
		FD_SET(e.cmdfd, &rfds);
		int n = select(e.cmdfd + 1, &rfds, NULL, NULL, &tv);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0 || !FD_ISSET(e.cmdfd, &rfds))
			break;
		::ttyread(&e);
		if (ImGui::GetTime() >= deadline)
			break;
	}
#endif
}

int
Terminal::makeglyphfontspecs(ImwGlyphSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	int winx = s_borderpx + x * tw.cw;
	int winy = s_borderpx + y * tw.ch;
	int xp = winx;
	int numspecs = 0;

	for (int i = 0; i < len; i++)
	{
		Rune rune = glyphs[i].u;
		ushort mode = glyphs[i].mode;

		if (mode == ATTR_WDUMMY)
			continue;

		Font *fnt = &s_dc.font;
		if ((mode & ATTR_BOLD_FAINT) == ATTR_BOLD && (mode & ATTR_ITALIC))
			fnt = &s_dc.ibfont;
		else if ((mode & ATTR_BOLD_FAINT) == ATTR_BOLD)
			fnt = &s_dc.bfont;
		else if (mode & ATTR_ITALIC)
			fnt = &s_dc.ifont;

		int runewidth = tw.cw * ((mode & ATTR_WIDE) ? 2 : 1);

		if (numspecs >= 1024)
		{
			fprintf(stderr, "imgui_terminal: spec buffer overflow at row %d (cap %d)\n",
			    y, 1024);
			break;
		}

		specs[numspecs].font = fnt->match;
		specs[numspecs].src = fnt;
		specs[numspecs].codepoint = rune;
		specs[numspecs].x = xp;
		specs[numspecs].y = winy;

		xp += runewidth;
		numspecs++;
	}

	return numspecs;
}

void
Terminal::drawglyphfontspecs(const ImwGlyphSpec *specs, Glyph base, int len, int x, int y)
{
	int charlen = len * ((base.mode & ATTR_WIDE) ? 2 : 1);
	int winx = s_borderpx + x * tw.cw;
	int winy = s_borderpx + y * tw.ch;
	int width = charlen * tw.cw;

	ImU32 fg = resolve_glyph_color(base.fg);
	ImU32 bg = resolve_glyph_color(base.bg);

	if (specs[0].src && (specs[0].src->badslant || specs[0].src->badweight))
		fg = colors[s_defaultattr];

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && between((int) base.fg, 0, 7))
	{
		fg = colors[base.fg + 8];
	}

	if (tw.mode & MODE_REVERSE)
	{
		if (fg == colors[defaultfg])
			fg = colors[defaultbg];
		else
			fg = (fg ^ 0x00ffffff) | 0xff000000;
		if (bg == colors[defaultbg])
			bg = colors[defaultfg];
		else
			bg = (bg ^ 0x00ffffff) | 0xff000000;
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT)
	{
		uint8_t r = (fg >> IM_COL32_R_SHIFT) & 0xff;
		uint8_t g = (fg >> IM_COL32_G_SHIFT) & 0xff;
		uint8_t b = (fg >> IM_COL32_B_SHIFT) & 0xff;
		fg = IM_COL32(r / 2, g / 2, b / 2, 0xff);
	}

	if (base.mode & ATTR_REVERSE)
	{
		ImU32 t = fg;
		fg = bg;
		bg = t;
	}

	if ((base.mode & ATTR_BLINK) && (tw.mode & MODE_BLINK))
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	const int gutter_right =
	    (tw.w < s_borderpx + tw.tw + s_borderpx) ? tw.w : s_borderpx + tw.tw + s_borderpx;
	const int gutter_bottom =
	    (tw.h < s_borderpx + tw.th + s_borderpx) ? tw.h : s_borderpx + tw.th + s_borderpx;
	if (x == 0)
		clear_rect(0, (y == 0) ? 0 : winy, s_borderpx, winy + tw.ch, bg);
	if (winx + width >= s_borderpx + tw.tw)
		clear_rect(winx + width, (y == 0) ? 0 : winy, gutter_right, winy + tw.ch, bg);
	if (y == 0)
		clear_rect(winx, 0, winx + width, s_borderpx, bg);
	if (winy + tw.ch >= s_borderpx + tw.th)
		clear_rect(winx, winy + tw.ch, winx + width, gutter_bottom, bg);

	ImVec2 p0((float) winx, (float) winy);
	ImVec2 p1((float) (winx + width), (float) (winy + tw.ch));
	emit_rect(p0, p1, bg);

	emit_push_clip(p0, p1);

	for (int i = 0; i < len; i++)
	{
		const ImwGlyphSpec *s = &specs[i];
		char buf[8];
		int n = (int) utf8encode(s->codepoint, buf);
		ImVec2 gp((float) s->x, (float) s->y);
#ifdef _WIN32
		if ((base.mode & ATTR_WIDE) && s->codepoint > 0xFFFF)
		{
			ImFontBaked *baked = s->font->GetFontBaked(s_dc.font.pxsize);
			if (baked)
			{
				const ImFontGlyph *gl = baked->FindGlyph((ImWchar) s->codepoint);
				if (gl)
				{
					float sc = s_dc.font.pxsize / baked->Size;
					float gw = (gl->X1 - gl->X0) * sc;
					float gh = (gl->Y1 - gl->Y0) * sc;
					float cw2 = (float) (tw.cw * 2);
					float ch1 = (float) tw.ch;
					gp.x += (cw2 - gw) * 0.5f - gl->X0 * sc;
					gp.y += (ch1 - gh) * 0.5f - gl->Y0 * sc;
				}
			}
		}
#endif
		emit_text(s->font, gp, fg, buf, n);
	}

	if (base.mode & ATTR_UNDERLINE)
	{
		float uy = (float) (winy + s_dc.font.ascent + 1);
		emit_rect(ImVec2(p0.x, uy), ImVec2(p1.x, uy + 1.0f), fg);
	}

	if (base.mode & ATTR_STRUCK)
	{
		float sy = (float) winy + 2.0f * (float) s_dc.font.ascent / 3.0f;
		emit_rect(ImVec2(p0.x, sy), ImVec2(p1.x, sy + 1.0f), fg);
	}

	emit_pop_clip();
}

void
Terminal::drawglyph_internal(Glyph g, int x, int y)
{
	ImwGlyphSpec spec;
	int numspecs = makeglyphfontspecs(&spec, &g, 1, x, y);
	if (numspecs > 0)
		drawglyphfontspecs(&spec, g, numspecs, x, y);
}

/* ----------------------------------------------------------------------
   Font loading: file-scope helpers. The chosen pattern + variants land
   in shared s_dc. load_fonts_once() guards against re-entry.
   ---------------------------------------------------------------------- */

static int
imw_load_one_font(Font *f, FcPattern *src_pattern, double pxsize)
{
	memset(f, 0, sizeof(*f));
	f->pxsize = (float) pxsize;

	FcPattern *p = FcPatternDuplicate(src_pattern);
	FcConfigSubstitute(NULL, p, FcMatchPattern);
	FcDefaultSubstitute(p);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, p, &result);
	if (!match)
	{
		FcPatternDestroy(p);
		fprintf(stderr, "imgui_terminal: FcFontMatch failed\n");
		return 0;
	}

	FcChar8 *file = NULL;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file)
	{
		FcPatternDestroy(match);
		FcPatternDestroy(p);
		fprintf(stderr, "imgui_terminal: matched font has no FC_FILE\n");
		return 0;
	}

	int wantv, gotv;
	if (FcPatternGetInteger(p, FC_SLANT, 0, &wantv) == FcResultMatch &&
	    FcPatternGetInteger(match, FC_SLANT, 0, &gotv) == FcResultMatch && wantv != gotv)
	{
		f->badslant = 1;
	}
	if (FcPatternGetInteger(p, FC_WEIGHT, 0, &wantv) == FcResultMatch &&
	    FcPatternGetInteger(match, FC_WEIGHT, 0, &gotv) == FcResultMatch && wantv != gotv)
	{
		f->badweight = 1;
	}

	ImFontConfig cfg;
	cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor;
	f->match =
	    ImGui::GetIO().Fonts->AddFontFromFileTTF((const char *) file, (float) pxsize, &cfg);

	FcPatternDestroy(match);
	FcPatternDestroy(p);

	if (!f->match)
	{
		fprintf(stderr, "imgui_terminal: AddFontFromFileTTF failed\n");
		return 0;
	}
	return 1;
}

static void
imw_load_fallback(const char *fc_query, double pxsize)
{
	FcPattern *pat = FcNameParse((const FcChar8 *) fc_query);
	if (!pat)
	{
		fprintf(stderr, "imgui_terminal: fallback FcNameParse failed: %s\n", fc_query);
		return;
	}
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	FcPatternDestroy(pat);
	if (!match)
	{
		fprintf(stderr, "imgui_terminal: no fallback for '%s'\n", fc_query);
		return;
	}

	FcChar8 *file = NULL;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file)
	{
		FcPatternDestroy(match);
		fprintf(stderr, "imgui_terminal: fallback '%s' has no FC_FILE\n", fc_query);
		return;
	}

	ImFontConfig cfg;
	cfg.MergeMode = true;
	cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor | ImGuiFreeTypeLoaderFlags_Bitmap;

#ifndef _WIN32
	if (strstr(fc_query, "und-zsye") != NULL)
		cfg.RasterizerDensity = 20.0f / (float) pxsize;
#endif

	ImFont *merged =
	    ImGui::GetIO().Fonts->AddFontFromFileTTF((const char *) file, (float) pxsize, &cfg);
	if (!merged)
		fprintf(stderr, "imgui_terminal: fallback merge failed: %s (file=%s)\n", fc_query,
		    (const char *) file);

	FcPatternDestroy(match);
}

static void
imw_load_variant_fallbacks(double pxsize)
{
	imw_load_fallback(":lang=ja", pxsize);
	imw_load_fallback(":lang=und-zsye", pxsize);
}

void
Terminal::load_fonts_once()
{
	if (s_fonts_loaded)
		return;
#ifdef _WIN32
	{
		char exe_dir[1024];
		imw_get_exe_dir(exe_dir, sizeof exe_dir);
		if (exe_dir[0])
		{
			char fc_path[1024];
			snprintf(fc_path, sizeof fc_path, "%s\\fontconfig", exe_dir);
			_putenv_s("FONTCONFIG_PATH", fc_path);
		}
	}
#endif
	if (!FcInit())
		die("imgui_terminal: FcInit failed\n");

	ImGui::GetIO().Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());

	FcPattern *pattern = FcNameParse((const FcChar8 *) font);
	if (!pattern)
		die("imgui_terminal: failed to parse font pattern: %s\n", font);

	double pxsize = 16.0;
	FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &pxsize);

	if (imw_load_one_font(&s_dc.font, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (imw_load_one_font(&s_dc.ifont, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (imw_load_one_font(&s_dc.ibfont, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (imw_load_one_font(&s_dc.bfont, pattern, pxsize))
		imw_load_variant_fallbacks(pxsize);

	FcPatternDestroy(pattern);
	s_fonts_loaded = true;
}

static void
imw_derive_metrics_from_baked(Font *f)
{
	if (!f->match)
		return;
	ImFontBaked *baked = f->match->GetFontBaked(f->pxsize);
	if (!baked)
		return;
	f->ascent = (int) ceilf(baked->Ascent);
	f->descent = (int) ceilf(-baked->Descent);
	f->height = f->ascent + f->descent;

	float advance = baked->GetCharAdvance((ImWchar) 'M');
	if (advance <= 0.0f)
		advance = baked->FallbackAdvanceX;
	if (advance <= 0.0f)
		advance = (float) f->height * 0.6f;
	f->width = (int) ceilf(advance);
}

void
Terminal::finalize_metrics()
{
	imw_derive_metrics_from_baked(&s_dc.font);
	imw_derive_metrics_from_baked(&s_dc.bfont);
	imw_derive_metrics_from_baked(&s_dc.ifont);
	imw_derive_metrics_from_baked(&s_dc.ibfont);

	if (s_dc.font.width > 0 && s_dc.font.height > 0)
	{
		tw.cw = (int) ceilf(s_dc.font.width * s_cwscale);
		tw.ch = (int) ceilf(s_dc.font.height * s_chscale);
	}
	metrics_derived = true;
}

float
Terminal::get_font_size()
{
	return s_dc.font.pxsize;
}

void
Terminal::set_font_size(float px)
{
	if (px < 6.0f)
		px = 6.0f;
	if (px > 72.0f)
		px = 72.0f;
	s_dc.font.pxsize = px;
	s_dc.bfont.pxsize = px;
	s_dc.ifont.pxsize = px;
	s_dc.ibfont.pxsize = px;
	finalize_metrics();

	tw.tw = 0;
	tw.th = 0;
}

void
Terminal::handle_resize(ImVec2 avail)
{
	int new_cols = (int) avail.x / tw.cw;
	int new_rows = (int) avail.y / tw.ch;
	if (new_cols < 1)
		new_cols = 1;
	if (new_rows < 1)
		new_rows = 1;
	int cur_cols = tw.tw / tw.cw;
	int cur_rows = tw.th / tw.ch;
	if (new_cols == cur_cols && new_rows == cur_rows)
		return;

	tw.tw = new_cols * tw.cw;
	tw.th = new_rows * tw.ch;
	tw.w = (int) avail.x;
	tw.h = (int) avail.y;
	tresize(&e, new_cols, new_rows);

	ttyresize(&e, tw.tw, tw.th);

	row_ops.resize(new_rows);

	redraw(&e);
}

/* ----------------------------------------------------------------------
	Keyboard / mouse dispatch.
   ---------------------------------------------------------------------- */

void
Terminal::dispatch_keyboard()
{
	ImGuiIO &io = ImGui::GetIO();
	bool consumed_this_frame = false;

	for (const Shortcut *s = s_shortcuts; s < s_shortcuts + std::size(s_shortcuts); s++)
	{
		if (ImGui::Shortcut(s->chord, ImGuiInputFlags_RouteFocused))
		{
			s->func(this, &s->arg);
			consumed_this_frame = true;
			break;
		}
	}

	if (!consumed_this_frame)
	{
		for (const Key *kp = s_key; kp < s_key + std::size(s_key); kp++)
		{
			if (!ImGui::IsKeyPressed(kp->k, /*  repeat  */ true))
				continue;
			if (!imw_match_mods(kp->mods, io.KeyMods))
				continue;
			if ((tw.mode & MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
				continue;
			if ((tw.mode & MODE_NUMLOCK) && kp->appkey == 2)
				continue;
			if ((tw.mode & MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
				continue;
			ttywrite(&e, kp->s, strlen(kp->s), 1);
			consumed_this_frame = true;
			break;
		}
	}

	static_assert(ImGuiKey_Z - ImGuiKey_A == 25, "ImGuiKey_A..ImGuiKey_Z must be contiguous");
	if (!consumed_this_frame && io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper)
	{
		for (int k = ImGuiKey_A; k <= ImGuiKey_Z; k++)
		{
			if (ImGui::IsKeyPressed((ImGuiKey) k, /*  repeat  */ true))
			{
				char byte = (char) (k - ImGuiKey_A + 1);
				ttywrite(&e, &byte, 1, 1);
				consumed_this_frame = true;
				break;
			}
		}
	}

	static const struct
	{
		ImGuiKey key;
		char byte;
	} ctrl_c0_map[] = {
	    {ImGuiKey_Space, 0x00},
	    {ImGuiKey_LeftBracket, 0x1B},
	    {ImGuiKey_Backslash, 0x1C},
	    {ImGuiKey_RightBracket, 0x1D},
	};
	if (!consumed_this_frame && io.KeyCtrl && !io.KeyShift && !io.KeyAlt && !io.KeySuper)
	{
		for (size_t i = 0; i < sizeof(ctrl_c0_map) / sizeof(ctrl_c0_map[0]); i++)
		{
			if (ImGui::IsKeyPressed(ctrl_c0_map[i].key,
				/*  repeat  */ true))
			{
				ttywrite(&e, &ctrl_c0_map[i].byte, 1, 1);
				consumed_this_frame = true;
				break;
			}
		}
	}

	if (!consumed_this_frame)
	{
		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			unsigned int c = io.InputQueueCharacters[i];
			char buf[8];
			int n;

			if (io.KeyAlt && c < 0x80)
			{
				if (tw.mode & MODE_8BIT)
				{
					n = (int) utf8encode((Rune) (c | 0x80), buf);
				}
				else
				{
					buf[0] = '\033';
					buf[1] = (char) c;
					n = 2;
				}
			}
			else
			{
				n = (int) utf8encode((Rune) c, buf);
			}
			ttywrite(&e, buf, (size_t) n, 1);
		}
	}
}

void
Terminal::dispatch_mouse_report()
{
	ImGuiIO &io = ImGui::GetIO();

	ImVec2 mpos = ImGui::GetMousePos();
	int col, row;
	pixel_to_cell(mpos, &col, &row);

	int mods = 0;
	if (!(tw.mode & MODE_MOUSEX10))
	{
		mods = (io.KeyShift ? 4 : 0) + (io.KeyAlt ? 8 : 0) + (io.KeyCtrl ? 16 : 0);
	}

	static const int btn_to_code[3] = {0, 2, 1};

	auto emit = [&](int btn_code, bool release)
	{
		emit_mouse(btn_code, col, row, mods, release);
		last_motion_x = col;
		last_motion_y = row;
	};

	for (int b = 0; b < 3; b++)
	{
		if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(b))
			emit(btn_to_code[b], false);
	}
	for (int b = 0; b < 3; b++)
	{
		if (!ImGui::IsMouseReleased(b))
			continue;
		if (tw.mode & MODE_MOUSEX10)
			continue;
		emit(btn_to_code[b], true);
	}

	if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
	{
		int code = io.MouseWheel > 0 ? 64 : 65;
		emit(code, false);
	}

	if ((tw.mode & MODE_MOUSEMOTION) || (tw.mode & MODE_MOUSEMANY))
	{
		int btn_code = -1;

		static const int priority[3] = {0, 2, 1};
		for (int i = 0; i < 3; i++)
		{
			int b = priority[i];
			if (ImGui::IsMouseDown(b))
			{
				btn_code = btn_to_code[b];
				break;
			}
		}
		if (btn_code < 0 && (tw.mode & MODE_MOUSEMANY))
			btn_code = 3;

		if (btn_code >= 0 && (col != last_motion_x || row != last_motion_y))
		{
			emit(btn_code + 32, false);
		}
	}
}

void
Terminal::dispatch_mouse_select()
{
	ImGuiIO &io = ImGui::GetIO();

	if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		drag_sel_type = SEL_REGULAR;
		for (int t = 1; t < (int) std::size(s_selmasks); t++)
		{
			if (imw_match_mods(s_selmasks[t], io.KeyMods))
			{
				drag_sel_type = t;
				break;
			}
		}

		int snap = 0;
		int n = io.MouseClickedCount[ImGuiMouseButton_Left];
		if (n == 2)
			snap = SNAP_WORD;
		else if (n >= 3)
			snap = SNAP_LINE;

		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		pixel_to_cell(mpos, &col, &row);
		selstart(&e, col, row, snap);
		selecting = true;
	}
	if (selecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		pixel_to_cell(mpos, &col, &row);
		selextend(&e, col, row, drag_sel_type, 0);
	}
	if (selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		pixel_to_cell(mpos, &col, &row);
		selextend(&e, col, row, drag_sel_type, 1);
		selecting = false;
	}
}

void
Terminal::dispatch_mouse_mshortcuts()
{
	ImGuiIO &io = ImGui::GetIO();
	int state = io.KeyMods;

	auto walk = [&](int button, unsigned int release)
	{
		for (const MouseShortcut *ms = s_mshortcuts;
		    ms < s_mshortcuts + std::size(s_mshortcuts); ms++)
		{
			if (ms->release != release)
				continue;
			if (ms->button != button)
				continue;
			if (!imw_match_mods(ms->mods, state) &&
			    !imw_match_mods(ms->mods, state & ~s_forcemousemod))
				continue;
			ms->func(this, &ms->arg);
			return true;
		}
		return false;
	};

	for (int b = 0; b < 5; b++)
	{
		if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(b))
			walk(b, 0);
		if (ImGui::IsMouseReleased(b))
			walk(b, 1);
	}

	if (ImGui::IsItemHovered() && io.MouseWheel != 0.0f)
		walk(io.MouseWheel > 0 ? Terminal::IMW_MB_WHEELUP : Terminal::IMW_MB_WHEELDOWN, 0);
}

void
Terminal::dispatch_mouse()
{
	ImGuiIO &io = ImGui::GetIO();
	bool report_mouse = (tw.mode & MODE_MOUSE) && !io.KeyShift;

	if (report_mouse)
	{
		dispatch_mouse_report();
	}
	else
	{
		dispatch_mouse_select();
		dispatch_mouse_mshortcuts();
	}
}

/* ----------------------------------------------------------------------
   Public widget API
   ---------------------------------------------------------------------- */

bool
Terminal::is_alive() const
{
	return e.alive != 0;
}
bool
Terminal::is_transparent() const
{
	return transparent_bg;
}
void
Terminal::set_transparent(bool on)
{
	transparent_bg = on;
}

size_t
Terminal::ttyread()
{
	return ::ttyread(&e);
}

void
Terminal::init(int cols, int rows, char **argv)
{
	// Set host BEFORE any emulator call so the callbacks can resolve back to this instance
	e.host = this;

	tw.cw = 8;
	tw.ch = 16;
	tw.tw = cols * tw.cw;
	tw.th = rows * tw.ch;
	tw.w = tw.tw;
	tw.h = tw.th;
	tw.mode = MODE_VISIBLE | MODE_FOCUSED | MODE_NUMLOCK;
	tw.cursor = 2;

	load_fonts_once();
	e.alive = 1;
	on_loadcols();
	row_ops.resize(rows);
#ifndef _WIN32
	setenv("TERM_PROGRAM", "st-imgui", 1);
#endif
	tnew(&e, cols, rows);
	selinit(&e);

	static char zsh_name[] = "zsh";
	char *shell_argv[] = {zsh_name, (char *) "+o", (char *) "PROMPT_SP", NULL};
	char **child_argv = argv;
	if (!argv)
	{
		const char *sh = getenv("SHELL");
		if (sh && strstr(sh, "zsh"))
			child_argv = shell_argv;
	}
	int fd = ttynew(&e, NULL, NULL, NULL, child_argv);
	if (fd < 0)
	{
		/* ttynew already reported the failure via cb_die and marked the
		   emulator dead. Don't abort the whole app — this terminal simply
		   comes up inert and is_alive() returns false. */
		e.alive = 0;
	}
}

void
Terminal::draw_canvas()
{
	if (!metrics_derived)
		finalize_metrics();

	ImVec2 avail = ImGui::GetContentRegionAvail();
	handle_resize(avail);

	ImVec2 cp = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##term_canvas", avail,
	    ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
		ImGuiButtonFlags_MouseButtonMiddle | ImGuiButtonFlags_EnableNav);
	bool focused = ImGui::IsItemFocused();

	if (focused)
	{
		ImGui::SetNextFrameWantCaptureKeyboard(true);
		tw.mode |= MODE_FOCUSED;
	}
	else
	{
		tw.mode &= ~MODE_FOCUSED;
	}
	if ((tw.mode & MODE_FOCUS) && focused != was_focused)
		ttywrite(&e, focused ? "\033[I" : "\033[O", 3, 0);
	was_focused = focused;

	canvas_pos = cp;
	dl = ImGui::GetWindowDrawList();

	if (!transparent_bg)
	{
		ImU32 bg_col = colors[(tw.mode & MODE_REVERSE) ? defaultfg : defaultbg];
		ImVec2 bg_p1(canvas_pos.x + avail.x, canvas_pos.y + avail.y);
		dl->AddRectFilled(canvas_pos, bg_p1, bg_col);
	}

	pump_pty();

	tick_blink();

	if (focused && !(tw.mode & MODE_KBDLOCK))
		dispatch_keyboard();

	dispatch_mouse();

	if (metrics_derived)
		draw(&e);

	for (size_t y = 0; y < row_ops.size(); y++)
		replay_ops(row_ops[y], canvas_pos, dl);
	replay_ops(overlay_ops, canvas_pos, dl);
}

void
Terminal::draw_widget(const char *id)
{
	// Title with persistent ID.
	char label[300];
	snprintf(label, sizeof label, "%s%s", title, id ? id : "###term_widget");

	ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);

	if (ImGui::Begin(label))
		draw_canvas();
	ImGui::End();
}

void
Terminal::shutdown()
{
	if (e.cmdfd >= 0)
	{
		ttyhangup(&e);
		e.cmdfd = -1;
	}
}

Terminal::~Terminal() = default;

/* ----------------------------------------------------------------------
   JSON dump: verbatim serialization of the draw-op cache.
   ---------------------------------------------------------------------- */

const char *
Terminal::dump_font_name(ImFont *f)
{
	if (f == s_dc.font.match)
		return "regular";
	if (f == s_dc.bfont.match)
		return "bold";
	if (f == s_dc.ifont.match)
		return "italic";
	if (f == s_dc.ibfont.match)
		return "bold_italic";
	return "other";
}

void
Terminal::dump_color(FILE *out, ImU32 c)
{
	int r = (int) ((c >> 0) & 0xff);
	int g = (int) ((c >> 8) & 0xff);
	int b = (int) ((c >> 16) & 0xff);
	fprintf(out, "\"#%02x%02x%02x\"", r, g, b);
}

void
Terminal::dump_jstr(FILE *out, const char *s, int len)
{
	fputc('"', out);
	for (int i = 0; i < len; i++)
	{
		unsigned char c = (unsigned char) s[i];
		switch (c)
		{
			case '"':
				fputs("\\\"", out);
				break;
			case '\\':
				fputs("\\\\", out);
				break;
			case '\b':
				fputs("\\b", out);
				break;
			case '\f':
				fputs("\\f", out);
				break;
			case '\n':
				fputs("\\n", out);
				break;
			case '\r':
				fputs("\\r", out);
				break;
			case '\t':
				fputs("\\t", out);
				break;
			default:
				if (c < 0x20)
					fprintf(out, "\\u%04x", c);
				else
					fputc(c, out);
		}
	}
	fputc('"', out);
}

void
Terminal::dump_op(FILE *out, const DrawOp &op)
{
	switch (op.kind)
	{
		case DrawOp::RECT:
			fprintf(out,
			    "{\"kind\":\"RECT\",\"p0\":[%g,%g],\"p1\":[%g,%g],\"col\":", op.p0.x,
			    op.p0.y, op.p1.x, op.p1.y);
			dump_color(out, op.col);
			fputc('}', out);
			break;
		case DrawOp::TEXT:
			fprintf(
			    out, "{\"kind\":\"TEXT\",\"p0\":[%g,%g],\"col\":", op.p0.x, op.p0.y);
			dump_color(out, op.col);
			fprintf(out, ",\"font\":\"%s\",\"text\":", dump_font_name(op.font));
			dump_jstr(out, (const char *) op.bytes, op.len);
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

void
Terminal::dump_oplist(FILE *out, const std::vector<DrawOp> &ops)
{
	for (size_t i = 0; i < ops.size(); i++)
	{
		if (i)
			fputc(',', out);
		dump_op(out, ops[i]);
	}
}

void
Terminal::dump_json(FILE *out)
{
	/*  Force a full redraw so the per-row cache reflects the
	    post-processing state of Term, not whatever was cached partway
	    through. Tests rely on this. */
	redraw(&e);

	fprintf(out, "{\n");
	fprintf(out, "  \"cw\": %d,\n", tw.cw);
	fprintf(out, "  \"ch\": %d,\n", tw.ch);
	fprintf(out, "  \"tw\": %d,\n", tw.tw);
	fprintf(out, "  \"th\": %d,\n", tw.th);
	int dump_mode = tw.mode;
#ifdef _WIN32
	dump_mode &= ~MODE_FOCUS;
#endif
	fprintf(out, "  \"mode\": %d,\n", dump_mode);
	fprintf(out, "  \"cursor_shape\": %d,\n", tw.cursor);
	const char *dump_title = "Terminal";
	fprintf(out, "  \"title\": ");
	dump_jstr(out, dump_title, (int) strlen(dump_title));
	fprintf(out, ",\n");

	int rows = (int) row_ops.size();
	fprintf(out, "  \"rows\": [");
	for (int r = 0; r < rows; r++)
	{
		fputs(r ? ",\n    " : "\n    ", out);
		fprintf(out, "{\"row\":%d,\"ops\":[", r);
		dump_oplist(out, row_ops[r]);
		fputc(']', out);
		fputc('}', out);
	}
	fprintf(out, "\n  ],\n");

	fprintf(out, "  \"overlay\": [");
	dump_oplist(out, overlay_ops);
	fprintf(out, "]\n");
	fprintf(out, "}\n");
}

/* ----------------------------------------------------------------------
   cb_*: emulator callback implementations. One-line dispatchers that
   recover the owning Terminal* via e->host. Must have C linkage because
   emulator.c calls these by name.
   ---------------------------------------------------------------------- */

extern "C"
{

	void cb_bell(Emulator *e)
	{
		static_cast<Terminal *>(e->host)->on_bell();
	}
	void cb_clipcopy(Emulator *e)
	{
		static_cast<Terminal *>(e->host)->on_clipcopy();
	}
	void cb_die(Emulator *e, const char *msg)
	{
		static_cast<Terminal *>(e->host)->on_die(msg);
	}
	void cb_drawcursor(Emulator *e, int cx, int cy, Glyph g, int ox, int oy, Glyph og)
	{
		static_cast<Terminal *>(e->host)->on_drawcursor(cx, cy, g, ox, oy, og);
	}
	void cb_drawline(Emulator *e, Line l, int x1, int y, int x2)
	{
		static_cast<Terminal *>(e->host)->on_drawline(l, x1, y, x2);
	}
	void cb_finishdraw(Emulator *e)
	{
		static_cast<Terminal *>(e->host)->on_finishdraw();
	}
	void cb_loadcols(Emulator *e)
	{
		static_cast<Terminal *>(e->host)->on_loadcols();
	}
	int cb_setcolorname(Emulator *e, int x, const char *n)
	{
		return static_cast<Terminal *>(e->host)->on_setcolorname(x, n);
	}
	int cb_getcolor(Emulator *e, int x, unsigned char *r, unsigned char *g, unsigned char *b)
	{
		return static_cast<Terminal *>(e->host)->on_getcolor(x, r, g, b);
	}
	void cb_seticontitle(Emulator *e, char *s)
	{
		static_cast<Terminal *>(e->host)->on_seticontitle(s);
	}
	void cb_settitle(Emulator *e, char *s)
	{
		static_cast<Terminal *>(e->host)->on_settitle(s);
	}
	int cb_setcursor(Emulator *e, int c)
	{
		return static_cast<Terminal *>(e->host)->on_setcursor(c);
	}
	void cb_setmode(Emulator *e, int s, unsigned int f)
	{
		static_cast<Terminal *>(e->host)->on_setmode(s, f);
	}
	void cb_setpointermotion(Emulator *e, int x)
	{
		static_cast<Terminal *>(e->host)->on_setpointermotion(x);
	}
	void cb_setsel(Emulator *e, char *s)
	{
		static_cast<Terminal *>(e->host)->on_setsel(s);
	}
	int cb_startdraw(Emulator *e)
	{
		return static_cast<Terminal *>(e->host)->on_startdraw();
	}
	void cb_ximspot(Emulator *e, int x, int y)
	{
		static_cast<Terminal *>(e->host)->on_ximspot(x, y);
	}
}

DC Terminal::s_dc;
bool Terminal::s_fonts_loaded = false;

void
Terminal::s_clipcopy(Terminal *t, const Arg *)
{
	char *sel = getsel(&t->e);
	if (sel)
	{
		ImGui::SetClipboardText(sel);
		free(sel);
	}
}
void
Terminal::s_clippaste(Terminal *t, const Arg *)
{
	const char *txt = ImGui::GetClipboardText();
	if (!txt || !*txt)
		return;
	size_t n = strlen(txt);
	if (t->tw.mode & MODE_BRCKTPASTE)
		ttywrite(&t->e, "\033[200~", 6, 0);
	ttywrite(&t->e, txt, n, 0);
	if (t->tw.mode & MODE_BRCKTPASTE)
		ttywrite(&t->e, "\033[201~", 6, 0);
}
void
Terminal::s_selpaste(Terminal *t, const Arg *)
{
	s_clippaste(t, NULL);
}
void
Terminal::s_numlock(Terminal *t, const Arg *)
{
	t->tw.mode ^= MODE_NUMLOCK;
}
void
Terminal::s_ttysend(Terminal *t, const Arg *a)
{
	if (a && a->s)
		ttywrite(&t->e, a->s, strlen(a->s), 1);
}
void
Terminal::s_printscreen(Terminal *t, const Arg *a)
{
	::printscreen(&t->e, a);
}
void
Terminal::s_toggleprinter(Terminal *t, const Arg *a)
{
	::toggleprinter(&t->e, a);
}

/* ---- Terminal ---- */
