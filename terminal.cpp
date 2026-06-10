/* See LICENSE for license details.

   terminal.cpp - implementation for terminal.h. Combines the st-based
   emulator core (PTY + escape-sequence parser), the ImGui rendering /
   input adapter, all in one class.
   Based on suckless st (suckless.org/st).
*/

#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000006 /* NTDDI_WIN10_RS5, ConPTY */
#endif
#endif

#include "terminal.h"

#include "imgui_freetype.h"
#include <fontconfig/fontconfig.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <algorithm>
#include <iterator>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
#include <io.h>
#include <sys/types.h>
#else
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#endif

#ifndef _WIN32
#if defined(__linux)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#endif
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#define LEN(a) (sizeof(a) / sizeof(a[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define DEFAULT(a, b) (a) = (a) ? (a) : (b)
#define LIMIT(x, a, b) (x) = (x)<(a) ? (a) : (x)>(b) ? (b) : (x)
#define TRUECOLOR(r, g, b) (1 << 24 | (r) << 16 | (g) << 8 | (b))

#define IS_SET(flag) ((term.mode & (flag)) != 0)

#define UTF_INVALID 0xFFFD

#ifdef _WIN32
static int
wcwidth(unsigned int ucs)
{
	WORD type = 0;
	wchar_t wc;

	if (ucs == 0)
		return 0;
	if (ucs < 32 || (ucs >= 0x7f && ucs < 0xa0))
		return -1;

	// Combining marks -> 0. BMP only; GetStringTypeW takes UTF-16.
	if (ucs <= 0xFFFF)
	{
		wc = (wchar_t) ucs;
		if (GetStringTypeW(CT_CTYPE3, &wc, 1, &type) && (type & C3_NONSPACING))
			return 0;
	}

	// East Asian Wide / Fullwidth ranges -> 2.
	if ((ucs >= 0x1100 && ucs <= 0x115F) ||	  /* Hangul Jamo init     */
	    (ucs >= 0x2E80 && ucs <= 0x303E) ||	  /* CJK Radicals/Symbols */
	    (ucs >= 0x3041 && ucs <= 0x33FF) ||	  /* Hiragana -> CJK Compat*/
	    (ucs >= 0x3400 && ucs <= 0x4DBF) ||	  /* CJK Ext A            */
	    (ucs >= 0x4E00 && ucs <= 0x9FFF) ||	  /* CJK Unified          */
	    (ucs >= 0xA000 && ucs <= 0xA4CF) ||	  /* Yi                   */
	    (ucs >= 0xAC00 && ucs <= 0xD7A3) ||	  /* Hangul Syllables     */
	    (ucs >= 0xF900 && ucs <= 0xFAFF) ||	  /* CJK Compat           */
	    (ucs >= 0xFE10 && ucs <= 0xFE19) ||	  /* Vertical forms       */
	    (ucs >= 0xFE30 && ucs <= 0xFE6F) ||	  /* CJK Compat Forms     */
	    (ucs >= 0xFF00 && ucs <= 0xFF60) ||	  /* Fullwidth Forms      */
	    (ucs >= 0xFFE0 && ucs <= 0xFFE6) ||	  /* Fullwidth Signs      */
	    (ucs >= 0x1F300 && ucs <= 0x1FBFF) || /* Emoji + Pictographic */
	    (ucs >= 0x20000 && ucs <= 0x3FFFD))	  /* CJK Ext B-G          */
		return 2;

	return 1;
}
#endif

// Arbitrary sizes
#define UTF_SIZ 4
#define ESC_BUF_SIZ (128 * UTF_SIZ)
#define ESC_ARG_SIZ 16
#define STR_BUF_SIZ ESC_BUF_SIZ
#define STR_ARG_SIZ ESC_ARG_SIZ

// macros
#define ISCONTROLC0(c) (between(c, 0, 0x1f) || (c) == 0x7f)
#define ISCONTROLC1(c) (between(c, 0x80, 0x9f))
#define ISCONTROL(c) (ISCONTROLC0(c) || ISCONTROLC1(c))
#define ISDELIM(u) (u && wcschr(worddelimiters, u))

enum term_mode
{
	MODE_WRAP = 1 << 0,
	MODE_INSERT = 1 << 1,
	MODE_ALTSCREEN = 1 << 2,
	MODE_CRLF = 1 << 3,
	MODE_ECHO = 1 << 4,
	MODE_PRINT = 1 << 5,
	MODE_UTF8 = 1 << 6,
};

enum cursor_movement
{
	CURSOR_SAVE,
	CURSOR_LOAD
};

enum cursor_state
{
	CURSOR_DEFAULT = 0,
	CURSOR_WRAPNEXT = 1,
	CURSOR_ORIGIN = 2
};

enum charset
{
	CS_GRAPHIC0,
	CS_GRAPHIC1,
	CS_UK,
	CS_USA,
	CS_MULTI,
	CS_GER,
	CS_FIN
};

enum escape_state
{
	ESC_START = 1,
	ESC_CSI = 2,
	ESC_STR = 4, /* DCS, OSC, PM, APC */
	ESC_ALTCHARSET = 8,
	ESC_STR_END = 16, /* a final string was encountered */
	ESC_TEST = 32,	  /* Enter in test mode */
	ESC_UTF8 = 64,
};

#ifndef _WIN32
static void
execsh(char *, char **);
static void
stty(char **);
static void
sigchld(int);
#endif

static size_t
utf8decode(const char *, Rune *, size_t);
static Rune
utf8decodebyte(char, size_t *);
static char
utf8encodebyte(Rune, size_t);
static size_t
utf8validate(Rune *, size_t);

static char *
base64dec(const char *);
static char
base64dec_getc(const char **);

static ssize_t
xwrite(int, const char *, size_t);

/* Per-instance state lives on the Terminal object. UTF tables below
   are read-only constants, they stay file-scope. */

static const uchar utfbyte[UTF_SIZ + 1] = {0x80, 0, 0xC0, 0xE0, 0xF0};
static const uchar utfmask[UTF_SIZ + 1] = {0xC0, 0x80, 0xE0, 0xF0, 0xF8};
static const Rune utfmin[UTF_SIZ + 1] = {0, 0, 0x80, 0x800, 0x10000};
static const Rune utfmax[UTF_SIZ + 1] = {0x10FFFF, 0x7F, 0x7FF, 0xFFFF, 0x10FFFF};

ssize_t
xwrite(int fd, const char *s, size_t len)
{
	size_t aux = len;
	ssize_t r;

	while (len > 0)
	{
#ifdef _WIN32
		r = _write(fd, s, (unsigned) len);
#else
		r = write(fd, s, len);
#endif
		if (r < 0)
			return r;
		len -= r;
		s += r;
	}

	return aux;
}

void *
xmalloc(size_t len)
{
	void *p;

	if (!(p = malloc(len)))
		die("malloc: %s\n", strerror(errno));

	return p;
}

void *
xrealloc(void *p, size_t len)
{
	if ((p = realloc(p, len)) == NULL)
		die("realloc: %s\n", strerror(errno));

	return p;
}

char *
xstrdup(const char *s)
{
	char *p;

	if ((p = strdup(s)) == NULL)
		die("strdup: %s\n", strerror(errno));

	return p;
}

size_t
utf8decode(const char *c, Rune *u, size_t clen)
{
	size_t i, j, len, type;
	Rune udecoded;

	*u = UTF_INVALID;
	if (!clen)
		return 0;
	udecoded = utf8decodebyte(c[0], &len);
	if (!between(len, 1, UTF_SIZ))
		return 1;
	for (i = 1, j = 1; i < clen && j < len; ++i, ++j)
	{
		udecoded = (udecoded << 6) | utf8decodebyte(c[i], &type);
		if (type != 0)
			return j;
	}
	if (j < len)
		return 0;
	*u = udecoded;
	utf8validate(u, len);

	return len;
}

Rune
utf8decodebyte(char c, size_t *i)
{
	for (*i = 0; *i < LEN(utfmask); ++(*i))
		if (((uchar) c & utfmask[*i]) == utfbyte[*i])
			return (uchar) c & ~utfmask[*i];

	return 0;
}

size_t
utf8encode(Rune u, char *c)
{
	size_t len, i;

	len = utf8validate(&u, 0);
	if (len > UTF_SIZ)
		return 0;

	for (i = len - 1; i != 0; --i)
	{
		c[i] = utf8encodebyte(u, 0);
		u >>= 6;
	}
	c[0] = utf8encodebyte(u, len);

	return len;
}

char
utf8encodebyte(Rune u, size_t i)
{
	return utfbyte[i] | (u & ~utfmask[i]);
}

size_t
utf8validate(Rune *u, size_t i)
{
	if (!between(*u, utfmin[i], utfmax[i]) || between(*u, 0xD800, 0xDFFF))
		*u = UTF_INVALID;
	for (i = 1; *u > utfmax[i]; ++i)
		;

	return i;
}

char
base64dec_getc(const char **src)
{
	while (**src && !isprint((unsigned char) **src))
		(*src)++;
	return **src ? *((*src)++) : '='; /* emulate padding if string ends */
}

char *
base64dec(const char *src)
{
	size_t in_len = strlen(src);
	char *result, *dst;
	/* clang-format off */
	static const char base64_digits[256] = {
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0, 62,  0,  0,  0, 63,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61,  0,  0,  0, -1,  0,  0,
		 0,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25,  0,  0,  0,  0,  0,
		 0, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
		 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	};
	/* clang-format on */

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = (char *) xmalloc(in_len / 4 * 3 + 1);
	while (*src)
	{
		int a = base64_digits[(unsigned char) base64dec_getc(&src)];
		int b = base64_digits[(unsigned char) base64dec_getc(&src)];
		int c = base64_digits[(unsigned char) base64dec_getc(&src)];
		int d = base64_digits[(unsigned char) base64dec_getc(&src)];

		// invalid input. 'a' can be -1, e.g. if src is "\n" (c-str)
		if (a == -1 || b == -1)
			break;

		*dst++ = (a << 2) | ((b & 0x30) >> 4);
		if (c == -1)
			break;
		*dst++ = ((b & 0x0f) << 4) | ((c & 0x3c) >> 2);
		if (d == -1)
			break;
		*dst++ = ((c & 0x03) << 6) | d;
	}
	*dst = '\0';
	return result;
}

void
Terminal::selinit()
{
	sel.mode = SEL_IDLE;
	sel.snap = 0;
	sel.ob.x = -1;
}

int
Terminal::tlinelen(int y)
{
	int i = term.col;

	if (term.line[y][i - 1].mode & ATTR_WRAP)
		return i;

	while (i > 0 && term.line[y][i - 1].u == ' ')
		--i;

	return i;
}

void
Terminal::selstart(int col, int row, int snap)
{
	selclear();
	sel.mode = SEL_EMPTY;
	sel.type = SEL_REGULAR;
	sel.alt = IS_SET(MODE_ALTSCREEN);
	sel.snap = snap;
	sel.oe.x = sel.ob.x = col;
	sel.oe.y = sel.ob.y = row;
	selnormalize();

	if (sel.snap != 0)
		sel.mode = SEL_READY;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
Terminal::selextend(int col, int row, int type, int done)
{
	int oldey, oldex, oldsby, oldsey, oldtype;

	if (sel.mode == SEL_IDLE)
		return;
	if (done && sel.mode == SEL_EMPTY)
	{
		selclear();
		return;
	}

	oldey = sel.oe.y;
	oldex = sel.oe.x;
	oldsby = sel.nb.y;
	oldsey = sel.ne.y;
	oldtype = sel.type;

	sel.oe.x = col;
	sel.oe.y = row;
	selnormalize();
	sel.type = type;

	if (oldey != sel.oe.y || oldex != sel.oe.x || oldtype != sel.type ||
	    sel.mode == SEL_EMPTY)
		tsetdirt(MIN(sel.nb.y, oldsby), MAX(sel.ne.y, oldsey));

	sel.mode = done ? SEL_IDLE : SEL_READY;
}

void
Terminal::selnormalize()
{
	int i;

	if (sel.type == SEL_REGULAR && sel.ob.y != sel.oe.y)
	{
		sel.nb.x = sel.ob.y < sel.oe.y ? sel.ob.x : sel.oe.x;
		sel.ne.x = sel.ob.y < sel.oe.y ? sel.oe.x : sel.ob.x;
	}
	else
	{
		sel.nb.x = MIN(sel.ob.x, sel.oe.x);
		sel.ne.x = MAX(sel.ob.x, sel.oe.x);
	}
	sel.nb.y = MIN(sel.ob.y, sel.oe.y);
	sel.ne.y = MAX(sel.ob.y, sel.oe.y);

	selsnap(&sel.nb.x, &sel.nb.y, -1);
	selsnap(&sel.ne.x, &sel.ne.y, +1);

	// expand selection over line breaks
	if (sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(sel.nb.y);
	if (i < sel.nb.x)
		sel.nb.x = i;
	if (tlinelen(sel.ne.y) <= sel.ne.x)
		sel.ne.x = term.col - 1;
}

int
Terminal::selected(int x, int y)
{
	if (sel.mode == SEL_EMPTY || sel.ob.x == -1 ||
	    sel.alt != IS_SET(MODE_ALTSCREEN))
		return 0;

	if (sel.type == SEL_RECTANGULAR)
		return between(y, sel.nb.y, sel.ne.y) && between(x, sel.nb.x, sel.ne.x);

	return between(y, sel.nb.y, sel.ne.y) && (y != sel.nb.y || x >= sel.nb.x) &&
	       (y != sel.ne.y || x <= sel.ne.x);
}

void
Terminal::selsnap(int *x, int *y, int direction)
{
	int newx, newy, xt, yt;
	int delim, prevdelim;
	const Glyph *gp, *prevgp;

	switch (sel.snap)
	{
		case SNAP_WORD:
			/*
		   Snap around if the word wraps around at the end or
		   beginning of a line.
		*/
			prevgp = &term.line[*y][*x];
			prevdelim = ISDELIM(prevgp->u);
			for (;;)
			{
				newx = *x + direction;
				newy = *y;
				if (!between(newx, 0, term.col - 1))
				{
					newy += direction;
					newx = (newx + term.col) % term.col;
					if (!between(newy, 0, term.row - 1))
						break;

					if (direction > 0)
						yt = *y, xt = *x;
					else
						yt = newy, xt = newx;
					if (!(term.line[yt][xt].mode & ATTR_WRAP))
						break;
				}

				if (newx >= tlinelen(newy))
					break;

				gp = &term.line[newy][newx];
				delim = ISDELIM(gp->u);
				if (!(gp->mode & ATTR_WDUMMY) &&
				    (delim != prevdelim || (delim && gp->u != prevgp->u)))
					break;

				*x = newx;
				*y = newy;
				prevgp = gp;
				prevdelim = delim;
			}
			break;
		case SNAP_LINE:
			/*
		   Snap around if the the previous line or the current one
		   has set ATTR_WRAP at its end. Then the whole next or
		   previous line will be selected.
		*/
			*x = (direction < 0) ? 0 : term.col - 1;
			if (direction < 0)
			{
				for (; *y > 0; *y += direction)
				{
					if (!(term.line[*y - 1][term.col - 1].mode &
						ATTR_WRAP))
					{
						break;
					}
				}
			}
			else if (direction > 0)
			{
				for (; *y < term.row - 1; *y += direction)
				{
					if (!(term.line[*y][term.col - 1].mode & ATTR_WRAP))
					{
						break;
					}
				}
			}
			break;
	}
}

char *
Terminal::getsel()
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen;
	const Glyph *gp, *last;

	if (sel.ob.x == -1)
		return NULL;

	bufsize = (term.col + 1) * (sel.ne.y - sel.nb.y + 1) * UTF_SIZ;
	ptr = str = (char *) xmalloc(bufsize);

	// append every set & selected glyph to the selection
	for (y = sel.nb.y; y <= sel.ne.y; y++)
	{
		if ((linelen = tlinelen(y)) == 0)
		{
			*ptr++ = '\n';
			continue;
		}

		if (sel.type == SEL_RECTANGULAR)
		{
			gp = &term.line[y][sel.nb.x];
			lastx = sel.ne.x;
		}
		else
		{
			gp = &term.line[y][sel.nb.y == y ? sel.nb.x : 0];
			lastx = (sel.ne.y == y) ? sel.ne.x : term.col - 1;
		}
		last = &term.line[y][MIN(lastx, linelen - 1)];
		while (last >= gp && last->u == ' ')
			--last;

		for (; gp <= last; ++gp)
		{
			if (gp->mode & ATTR_WDUMMY)
				continue;

			ptr += utf8encode(gp->u, ptr);
		}

		/*
		   Copy and pasting of line endings is inconsistent
		   in the inconsistent terminal and GUI world.
		   The best solution seems like to produce '\n' when
		   something is copied from st and convert '\n' to
		   '\r', when something to be pasted is received by
		   st.
		   FIXME: Fix the computer world.
		*/
		if ((y < sel.ne.y || lastx >= linelen) &&
		    (!(last->mode & ATTR_WRAP) || sel.type == SEL_RECTANGULAR))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void
Terminal::selclear()
{
	if (sel.ob.x == -1)
		return;
	sel.mode = SEL_IDLE;
	sel.ob.x = -1;
	tsetdirt(sel.nb.y, sel.ne.y);
}

void
die(const char *errstr, ...)
{
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);
	exit(1);
}

/* Per-instance fatal error. Unlike die(), this never terminates the process:
   it marks just this terminal dead and reports the message via
   handle_child_exit(), so one terminal failing cannot take down the others. Callers
   must return promptly after invoking it. */
void
Terminal::emu_die(const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	handle_child_exit(buf);
	alive = 0;
}

#ifdef _WIN32
/* ----------------------------------------------------------------------
   Windows ConPTY implementation
   ---------------------------------------------------------------------- */
unsigned __stdcall
Terminal::w32_monitor_thread(void *arg)
{
	Terminal *t = (Terminal *) arg;
	WaitForSingleObject(t->w32_proc, INFINITE);
	WaitForSingleObject(t->w32_reader_ready, 5000);
	if (t->w32_hpc)
	{
		ClosePseudoConsole(t->w32_hpc);
		t->w32_hpc = NULL;
	}
	return 0;
}

/* Build child command line from args[], quoting args with spaces.
   Returns 0 on success, -1 if the command line would overflow buf. */
int
Terminal::w32_build_cmdline(char *buf, size_t cap, char *cmd, char **args)
{
	size_t pos = 0;
	char **p;
	const char *shell;

	if (args && args[0])
	{
		for (p = args; *p; p++)
		{
			int quote = (strchr(*p, ' ') != NULL);
			size_t alen = strlen(*p);
			size_t need = alen + (quote ? 2 : 0) + (pos > 0 ? 1 : 0);

			if (pos + need + 1 > cap)
			{
				emu_die("ttynew: command line exceeds %zu bytes\n", cap);
				return -1;
			}
			if (pos > 0)
				buf[pos++] = ' ';
			if (quote)
				buf[pos++] = '"';
			memcpy(buf + pos, *p, alen);
			pos += alen;
			if (quote)
				buf[pos++] = '"';
		}
		buf[pos] = '\0';
		return 0;
	}

	shell = cmd ? cmd : getenv("COMSPEC");
	if (!shell)
		shell = "cmd.exe";
	if ((size_t) snprintf(buf, cap, "%s", shell) >= cap)
	{
		emu_die("ttynew: command line exceeds %zu bytes\n", cap);
		return -1;
	}
	return 0;
}

/* Spawn child attached to ConPTY via STARTUPINFOEXW.
   Returns 0 on success, -1 on failure (no process created). */
int
Terminal::w32_spawn_pcon_child(LPWSTR cmdline, HPCON hpc, HANDLE *out_proc)
{
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attr_sz = 0;

	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	si.StartupInfo.cb = sizeof(si);

	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
	si.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST) malloc(attr_sz);
	if (!si.lpAttributeList)
	{
		emu_die("malloc(%zu) for attribute list failed\n", (size_t) attr_sz);
		return -1;
	}
	if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_sz))
	{
		emu_die("InitializeProcThreadAttributeList failed: %lu\n", GetLastError());
		free(si.lpAttributeList);
		return -1;
	}
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
		hpc, sizeof(HPCON), NULL, NULL))
	{
		emu_die("UpdateProcThreadAttribute failed: %lu\n", GetLastError());
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
		return -1;
	}

	if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL,
		NULL, &si.StartupInfo, &pi))
	{
		emu_die("CreateProcessW failed: %lu\n", GetLastError());
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
		return -1;
	}

	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	CloseHandle(pi.hThread);
	*out_proc = pi.hProcess;
	return 0;
}

/* Close every ConPTY handle owned by this instance. Safe to call on a
   partially-initialized emulator (NULL handles are skipped). */
void
Terminal::w32_ttynew_cleanup()
{
	if (w32_proc)
	{
		TerminateProcess(w32_proc, 1);
		CloseHandle(w32_proc);
		w32_proc = NULL;
	}
	if (w32_hpc)
	{
		ClosePseudoConsole(w32_hpc);
		w32_hpc = NULL;
	}
	if (w32_pipe_in)
	{
		CloseHandle(w32_pipe_in);
		w32_pipe_in = NULL;
	}
	if (w32_pipe_out)
	{
		CloseHandle(w32_pipe_out);
		w32_pipe_out = NULL;
	}
	if (w32_reader_ready)
	{
		CloseHandle(w32_reader_ready);
		w32_reader_ready = NULL;
	}
}

int
Terminal::ttynew(const char *line, char *cmd, const char *out, char **args)
{
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE pipe_in_r, pipe_in_w, pipe_out_r, pipe_out_w;
	COORD size;
	HRESULT hr;
	char cmdline[4096];
	wchar_t *wcmd;
	int wlen;

	(void) line; /* Windows has no tty device path */

	// Optional I/O capture file (-o flag).
	if (out)
	{
		term.mode |= MODE_PRINT;
		iofd = (!strcmp(out, "-")) ? 1 : _open(out, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0)
			fprintf(stderr, "Error opening %s:%s\n", out, strerror(errno));
	}

	/* Two pipe pairs for ConPTY <-> child. SECURITY_ATTRIBUTES with
	   bInheritHandle=TRUE is required, ConPTY duplicates the inner
	   ends into conhost.exe internally. */
	if (!CreatePipe(&pipe_in_r, &pipe_in_w, &sa, 0))
	{
		emu_die("CreatePipe (in) failed: %lu\n", GetLastError());
		return -1;
	}
	if (!CreatePipe(&pipe_out_r, &pipe_out_w, &sa, 0))
	{
		emu_die("CreatePipe (out) failed: %lu\n", GetLastError());
		CloseHandle(pipe_in_r);
		CloseHandle(pipe_in_w);
		return -1;
	}

	// Pseudo-console attached to the inner pipe ends.
	size.X = (SHORT) (term.col ? term.col : 80);
	size.Y = (SHORT) (term.row ? term.row : 24);
	hr = CreatePseudoConsole(size, pipe_in_r, pipe_out_w, 0, &w32_hpc);
	if (FAILED(hr))
	{
		emu_die("CreatePseudoConsole failed: 0x%lx\n", (unsigned long) hr);
		CloseHandle(pipe_in_r);
		CloseHandle(pipe_in_w);
		CloseHandle(pipe_out_r);
		CloseHandle(pipe_out_w);
		return -1;
	}

	// ConPTY owns the inner ends; we keep the outer ends.
	CloseHandle(pipe_in_r);
	CloseHandle(pipe_out_w);
	w32_pipe_in = pipe_out_r;
	w32_pipe_out = pipe_in_w;

	/* TERM for ncurses-aware programs. MSYS=enable_pcon tells the
	   MSYS2/Cygwin runtime to cooperate with ConPTY rather than
	   bypassing it through its own PTY layer. */
	SetEnvironmentVariableA("TERM", termname);
	SetEnvironmentVariableA("MSYS", "enable_pcon");

	// Build the command line, convert UTF-8 -> UTF-16, spawn.
	if (w32_build_cmdline(cmdline, sizeof(cmdline), cmd, args) < 0)
	{
		w32_ttynew_cleanup(); /* w32_build_cmdline already reported */
		return -1;
	}
	wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
	if (wlen <= 0)
	{
		emu_die("MultiByteToWideChar failed: %lu\n", GetLastError());
		w32_ttynew_cleanup();
		return -1;
	}
	wcmd = (wchar_t *) malloc((size_t) wlen * sizeof(wchar_t));
	if (!wcmd)
	{
		emu_die("malloc for wide cmdline failed\n");
		w32_ttynew_cleanup();
		return -1;
	}
	MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmd, wlen);

	if (w32_spawn_pcon_child(wcmd, w32_hpc, &w32_proc) < 0)
	{
		free(wcmd);
		w32_ttynew_cleanup(); /* w32_spawn_pcon_child already reported */
		return -1;
	}
	free(wcmd);

	/* Reader-ready event must exist before the monitor thread starts
	   (the thread waits on it). See w32_monitor_thread comment. */
	w32_reader_ready = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!w32_reader_ready)
	{
		emu_die("CreateEvent failed: %lu\n", GetLastError());
		w32_ttynew_cleanup();
		return -1;
	}

	if (_beginthreadex(NULL, 0, w32_monitor_thread, this, 0, NULL) == 0)
	{
		emu_die("_beginthreadex(monitor) failed: %s\n", strerror(errno));
		w32_ttynew_cleanup();
		return -1;
	}

	// cmdfd is opaque on Windows, just needs to be >= 0.
	cmdfd = 1;
	alive = 1;
	return cmdfd;
}

#else /* !_WIN32, original POSIX implementation */

void
execsh(char *cmd, char **args)
{
	char *sh, *prog, *arg;
	const struct passwd *pw;

	errno = 0;
	if ((pw = getpwuid(getuid())) == NULL)
	{
		if (errno)
			die("getpwuid: %s\n", strerror(errno));
		else
			die("who are you?\n");
	}

	if ((sh = getenv("SHELL")) == NULL)
		sh = (pw->pw_shell[0]) ? pw->pw_shell : cmd;

	if (args)
	{
		prog = args[0];
		arg = NULL;
	}
	else if (scroll)
	{
		prog = scroll;
		arg = utmp ? utmp : sh;
	}
	else if (utmp)
	{
		prog = utmp;
		arg = NULL;
	}
	else
	{
		prog = sh;
		arg = NULL;
	}

	/* Spawn the child as a login shell (argv[0] = "-shellname") so
	   ~/.zprofile / .bash_profile is sourced. macOS GUI-launched apps
	   inherit launchd's minimal PATH; the login shell is what populates
	   it. Matches Terminal.app, iTerm2, Alacritty default behavior. */
	const char *base = strrchr(prog, '/');
	base = base ? base + 1 : prog;
	char argv0[256];
	snprintf(argv0, sizeof argv0, "-%s", base);
	if (args)
		args[0] = argv0;
	else
	{
		static char *fallback_args[3];
		fallback_args[0] = argv0;
		fallback_args[1] = arg;
		fallback_args[2] = NULL;
		args = fallback_args;
	}

	unsetenv("COLUMNS");
	unsetenv("LINES");
	unsetenv("TERMCAP");
	setenv("LOGNAME", pw->pw_name, 1);
	setenv("USER", pw->pw_name, 1);
	setenv("SHELL", sh, 1);
	setenv("HOME", pw->pw_dir, 1);
	setenv("TERM", termname, 1);

	signal(SIGCHLD, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGQUIT, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGALRM, SIG_DFL);

	execvp(prog, args);
	_exit(1);
}

void
sigchld(int a)
{
	(void) a;
	/* Deliberately does NOT reap here. A single shared SIGCHLD handler
	   cannot tell which Emulator a child belongs to, and waitpid(-1, ...)
	   would steal another instance's child-exit event. Instead each
	   instance reaps its own pid (see ttyread EOF and ttyhangup). This
	   handler exists only so SIGCHLD is delivered, interrupting a blocking
	   pselect() in ttywriteraw() so the caller notices the child is gone. */
}

void
stty(char **args)
{
	char cmd[_POSIX_ARG_MAX], **p, *q, *s;
	size_t n, siz;

	if ((n = strlen(stty_args)) > sizeof(cmd) - 1)
		die("incorrect stty parameters\n");
	memcpy(cmd, stty_args, n);
	q = cmd + n;
	siz = sizeof(cmd) - n;
	for (p = args; p && (s = *p); ++p)
	{
		if ((n = strlen(s)) > siz - 1)
			die("stty parameter length too long\n");
		*q++ = ' ';
		memcpy(q, s, n);
		q += n;
		siz -= n + 1;
	}
	*q = '\0';
	if (system(cmd) != 0)
		perror("Couldn't call stty");
}

int
Terminal::ttynew(const char *line, char *cmd, const char *out, char **args)
{
	int m, s;

	if (out)
	{
		term.mode |= MODE_PRINT;
		iofd = (!strcmp(out, "-")) ? 1 : open(out, O_WRONLY | O_CREAT, 0666);
		if (iofd < 0)
		{
			fprintf(stderr, "Error opening %s:%s\n", out, strerror(errno));
		}
	}

	if (line)
	{
		if ((cmdfd = open(line, O_RDWR)) < 0)
		{
			emu_die("open line '%s' failed: %s\n", line, strerror(errno));
			return -1;
		}
		dup2(cmdfd, 0);
		stty(args);
		return cmdfd;
	}

	// seems to work fine on linux, openbsd and freebsd
	if (openpty(&m, &s, NULL, NULL, NULL) < 0)
	{
		emu_die("openpty failed: %s\n", strerror(errno));
		return -1;
	}

	switch (pid = fork())
	{
		case -1:
			emu_die("fork failed: %s\n", strerror(errno));
			return -1;
		case 0:
			close(iofd);
			close(m);
			setsid(); /* create a new process group */
			dup2(s, 0);
			dup2(s, 1);
			dup2(s, 2);
			if (ioctl(s, TIOCSCTTY, NULL) < 0)
				die("ioctl TIOCSCTTY failed: %s\n", strerror(errno));
			if (s > 2)
				close(s);
#ifdef __OpenBSD__
			if (pledge("stdio getpw proc exec", NULL) == -1)
				die("pledge\n");
#endif
			execsh(cmd, args);
			break;
		default:
#ifdef __OpenBSD__
			if (pledge("stdio rpath tty proc", NULL) == -1)
			{
				emu_die("pledge\n");
				return -1;
			}
#endif
			close(s);
			cmdfd = m;
			alive = 1;
			signal(SIGCHLD, sigchld);
			break;
	}
	return cmdfd;
}
#endif /* !_WIN32 */

#ifdef _WIN32
size_t
Terminal::ttyread()
{
	int written;
	if (!w32_reader_signaled)
	{
		SetEvent(w32_reader_ready);
		w32_reader_signaled = 1;
	}

	DWORD dwRead = 0;
	if (!ReadFile(w32_pipe_in, read_buf + read_buflen,
		LEN(read_buf) - read_buflen, &dwRead, NULL) ||
	    dwRead == 0)
	{
		// Pipe broken by ClosePseudoConsole. Drain residual.
		for (;;)
		{
			if (!ReadFile(w32_pipe_in, read_buf + read_buflen,
				LEN(read_buf) - read_buflen, &dwRead, NULL) ||
			    dwRead == 0)
				break;
			read_buflen += (int) dwRead;
			written = twrite(read_buf, read_buflen, 0);
			read_buflen -= written;
			if (read_buflen > 0)
				memmove(read_buf, read_buf + written, read_buflen);
		}
		alive = 0;
		return 0;
	}
	read_buflen += (int) dwRead;
	written = twrite(read_buf, read_buflen, 0);
	read_buflen -= written;
	if (read_buflen > 0)
		memmove(read_buf, read_buf + written, read_buflen);
	return (size_t) dwRead;
}
#else
	size_t
Terminal::ttyread()
{
	int ret, written;

	ret = read(cmdfd, read_buf + read_buflen, LEN(read_buf) - read_buflen);

	switch (ret)
	{
		case 0:
			/* EOF on the pty master: the child has closed its end, i.e.
			   it has exited. Reap this instance's own child so it does
			   not linger as a zombie. */
			alive = 0;
			if (pid > 0)
			{
				waitpid(pid, NULL, WNOHANG);
				pid = 0;
			}
			handle_child_exit("shell exited\n");
			if (cmdfd >= 0)
			{
				close(cmdfd);
				cmdfd = -1;
			}
			return 0;
		case -1:
			emu_die("couldn't read from shell: %s\n", strerror(errno));
			return 0;
		default:
			read_buflen += ret;
			written = twrite(read_buf, read_buflen, 0);
			read_buflen -= written;
			// keep any incomplete UTF-8 byte sequence for the next call
			if (read_buflen > 0)
				memmove(read_buf, read_buf + written, read_buflen);
			return ret;
	}
}
#endif

void
Terminal::ttywrite(const char *s, size_t n, int may_echo)
{
	const char *next;

	/* User input resets the cursor blink timer so the cursor appears
	   immediately, matching how every cursor-blink UI works. */
	if (may_echo && cursor_blinking)
	{
		cursor_blink_on = true;
		cursor_blink_timer = ImGui::GetTime() * 1000.0;
	}

	if (may_echo && IS_SET(MODE_ECHO))
		twrite(s, n, 1);

	if (!IS_SET(MODE_CRLF))
	{
		ttywriteraw(s, n);
		return;
	}

	// This is similar to how the kernel handles ONLCR for ttys
	while (n > 0)
	{
		if (*s == '\r')
		{
			next = s + 1;
			ttywriteraw("\r\n", 2);
		}
		else
		{
			next = (const char *) memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

void
Terminal::ttywriteraw(const char *s, size_t n)
{
#ifdef _WIN32
	/* Windows ConPTY: WriteFile on the pipe. Simpler than the Unix
	   pselect() dance because ConPTY handles flow control internally. */
	while (n > 0)
	{
		DWORD written = 0;
		if (!WriteFile(w32_pipe_out, s, (DWORD) n, &written, NULL))
		{
			emu_die("write error on tty: %lu\n", GetLastError());
			return;
		}
		n -= written;
		s += written;
	}
#else
	fd_set wfd, rfd;
	ssize_t r;
	size_t lim = 256;

	/*
	   Remember that we are using a pty, which might be a modem line.
	   Writing too much will clog the line. That's why we are doing this
	   dance.
	   FIXME: Migrate the world to Plan 9.
	*/
	while (n > 0)
	{
		FD_ZERO(&wfd);
		FD_ZERO(&rfd);
		FD_SET(cmdfd, &wfd);
		FD_SET(cmdfd, &rfd);

		// Check if we can write.
		if (pselect(cmdfd + 1, &rfd, &wfd, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR)
				continue;
			emu_die("select failed: %s\n", strerror(errno));
			return;
		}
		if (FD_ISSET(cmdfd, &wfd))
		{
			/*
			   Only write the bytes written by ttywrite() or the
			   default of 256. This seems to be a reasonable value
			   for a serial line. Bigger values might clog the I/O.
			*/
			if ((r = write(cmdfd, s, (n < lim) ? n : lim)) < 0)
				goto write_error;
			if (r < n)
			{
				/*
				   We weren't able to write out everything.
				   This means the buffer is getting full
				   again. Empty it.
				*/
				if (n < lim)
					lim = ttyread();
				n -= r;
				s += r;
			}
			else
			{
				// All bytes have been written.
				break;
			}
		}
		if (FD_ISSET(cmdfd, &rfd))
			lim = ttyread();
	}
	return;

write_error:
	emu_die("write error on tty: %s\n", strerror(errno));
#endif
}

void
Terminal::ttyresize(int tw, int th)
{
#ifdef _WIN32
	COORD size;
	size.X = (SHORT) term.col;
	size.Y = (SHORT) term.row;
	if (w32_hpc)
		ResizePseudoConsole(w32_hpc, size);
#else
	struct winsize w;

	w.ws_row = term.row;
	w.ws_col = term.col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
#endif
}

void
Terminal::ttyhangup()
{
#ifdef _WIN32
	if (w32_proc)
	{
		TerminateProcess(w32_proc, 1);
		CloseHandle(w32_proc);
		w32_proc = NULL;
	}
	if (w32_hpc)
	{
		ClosePseudoConsole(w32_hpc);
		w32_hpc = NULL;
	}
#else
	// Send SIGHUP to shell, then reap this instance's own child so it
	// does not become a zombie (the shared sigchld handler no longer reaps).
	if (pid > 0)
	{
		kill(pid, SIGHUP);
		waitpid(pid, NULL, 0);
		pid = 0;
		alive = 0;
	}
#endif
}

int
Terminal::tattrset(int attr)
{
	int i, j;

	for (i = 0; i < term.row - 1; i++)
	{
		for (j = 0; j < term.col - 1; j++)
		{
			if (term.line[i][j].mode & attr)
				return 1;
		}
	}

	return 0;
}

void
Terminal::tsetdirt(int top, int bot)
{
	int i;

	if (term.row <= 0)
		return;

	LIMIT(top, 0, term.row - 1);
	LIMIT(bot, 0, term.row - 1);

	for (i = top; i <= bot; i++)
		term.dirty[i] = 1;
}

void
Terminal::tsetdirtattr(int attr)
{
	int i, j;

	for (i = 0; i < term.row - 1; i++)
	{
		for (j = 0; j < term.col - 1; j++)
		{
			if (term.line[i][j].mode & attr)
			{
				tsetdirt(i, i);
				break;
			}
		}
	}
}

void
Terminal::tfulldirt()
{
	tsetdirt(0, term.row - 1);
}

void
Terminal::tcursor(int mode)
{
	int alt = IS_SET(MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE)
	{
		saved_cursor[alt] = term.c;
	}
	else if (mode == CURSOR_LOAD)
	{
		term.c = saved_cursor[alt];
		tmoveto(saved_cursor[alt].x, saved_cursor[alt].y);
	}
}

void
Terminal::treset()
{
	uint i;

	term.c = TCursor{};
	term.c.attr.mode = ATTR_NULL;
	term.c.attr.fg = defaultfg;
	term.c.attr.bg = defaultbg;
	term.c.x = 0;
	term.c.y = 0;
	term.c.state = CURSOR_DEFAULT;

	memset(term.tabs, 0, term.col * sizeof(*term.tabs));
	for (i = tabspaces; i < term.col; i += tabspaces)
		term.tabs[i] = 1;
	term.top = 0;
	term.bot = term.row - 1;
	term.mode = MODE_WRAP | MODE_UTF8;
	memset(term.trantbl, CS_USA, sizeof(term.trantbl));
	term.charset = 0;

	for (i = 0; i < 2; i++)
	{
		tmoveto(0, 0);
		tcursor(CURSOR_SAVE);
		tclearregion(0, 0, term.col - 1, term.row - 1);
		tswapscreen();
	}
}

void
Terminal::tnew(int col, int row)
{
	term = Term{};
	term.c.attr.fg = defaultfg;
	term.c.attr.bg = defaultbg;
	tresize(col, row);
	treset();
}

void
Terminal::tswapscreen()
{
	Line *tmp = term.line;

	term.line = term.alt;
	term.alt = tmp;
	term.mode ^= MODE_ALTSCREEN;
	tfulldirt();
}

void
Terminal::tscrolldown(int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot - orig + 1);

	tsetdirt(orig, term.bot - n);
	tclearregion(0, term.bot - n + 1, term.col - 1, term.bot);

	for (i = term.bot; i >= orig + n; i--)
	{
		temp = term.line[i];
		term.line[i] = term.line[i - n];
		term.line[i - n] = temp;
	}

	selscroll(orig, n);
}

void
Terminal::tscrollup(int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, term.bot - orig + 1);

	tclearregion(0, orig, term.col - 1, orig + n - 1);
	tsetdirt(orig + n, term.bot);

	for (i = orig; i <= term.bot - n; i++)
	{
		temp = term.line[i];
		term.line[i] = term.line[i + n];
		term.line[i + n] = temp;
	}

	selscroll(orig, -n);
}

void
Terminal::selscroll(int orig, int n)
{
	if (sel.ob.x == -1 || sel.alt != IS_SET(MODE_ALTSCREEN))
		return;

	if (between(sel.nb.y, orig, term.bot) != between(sel.ne.y, orig, term.bot))
	{
		selclear();
	}
	else if (between(sel.nb.y, orig, term.bot))
	{
		sel.ob.y += n;
		sel.oe.y += n;
		if (sel.ob.y < term.top || sel.ob.y > term.bot ||
		    sel.oe.y < term.top || sel.oe.y > term.bot)
		{
			selclear();
		}
		else
		{
			selnormalize();
		}
	}
}

void
Terminal::tnewline(int first_col)
{
	int y = term.c.y;

	if (y == term.bot)
	{
		tscrollup(term.top, 1);
	}
	else
	{
		y++;
	}
	tmoveto(first_col ? 0 : term.c.x, y);
}

void
Terminal::csiparse()
{
	char *p = csi.buf, *np;
	long int v;
	int sep = ';'; /* colon or semi-colon, but not both */

	csi.narg = 0;
	if (*p == '?')
	{
		csi.priv = 1;
		p++;
	}

	csi.buf[csi.len] = '\0';
	while (p < csi.buf + csi.len)
	{
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		csi.arg[csi.narg++] = v;
		p = np;
		if (sep == ';' && *p == ':')
			sep = ':'; /* allow override to colon once */
		if (*p != sep || csi.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	csi.mode[0] = *p++;
	csi.mode[1] = (p < csi.buf + csi.len) ? *p : '\0';
}

// for absolute user moves, when decom is set
void
Terminal::tmoveato(int x, int y)
{
	tmoveto(x, y + ((term.c.state & CURSOR_ORIGIN) ? term.top : 0));
}

void
Terminal::tmoveto(int x, int y)
{
	int miny, maxy;

	if (term.c.state & CURSOR_ORIGIN)
	{
		miny = term.top;
		maxy = term.bot;
	}
	else
	{
		miny = 0;
		maxy = term.row - 1;
	}
	term.c.state &= ~CURSOR_WRAPNEXT;
	term.c.x = LIMIT(x, 0, term.col - 1);
	term.c.y = LIMIT(y, miny, maxy);
}

void
Terminal::tsetchar(Rune u, const Glyph *attr, int x, int y)
{
	/* clang-format off */
	static const char *vt100_0[62] = { /* 0x41 - 0x7e */
		"↑", "↓", "→", "←", "█", "▚", "☃", /* A - G */
		0, 0, 0, 0, 0, 0, 0, 0, /* H - O */
		0, 0, 0, 0, 0, 0, 0, 0, /* P - W */
		0, 0, 0, 0, 0, 0, 0, " ", /* X - _ */
		"◆", "▒", "␉", "␌", "␍", "␊", "°", "±", /* ` - g */
		"␤", "␋", "┘", "┐", "┌", "└", "┼", "⎺", /* h - o */
		"⎻", "─", "⎼", "⎽", "├", "┤", "┴", "┬", /* p - w */
		"│", "≤", "≥", "π", "≠", "£", "·", /* x - ~ */
	};
	/* clang-format on */

	/*
	   The table is proudly stolen from rxvt.
	*/
	if (term.trantbl[term.charset] == CS_GRAPHIC0 && between(u, 0x41, 0x7e) &&
	    vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if (term.line[y][x].mode & ATTR_WIDE)
	{
		if (x + 1 < term.col)
		{
			term.line[y][x + 1].u = ' ';
			term.line[y][x + 1].mode &= ~ATTR_WDUMMY;
		}
	}
	else if (term.line[y][x].mode & ATTR_WDUMMY)
	{
		term.line[y][x - 1].u = ' ';
		term.line[y][x - 1].mode &= ~ATTR_WIDE;
	}

	term.dirty[y] = 1;
	term.line[y][x] = *attr;
	term.line[y][x].u = u;
}

void
Terminal::tclearregion(int x1, int y1, int x2, int y2)
{
	int x, y, temp;
	Glyph *gp;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, term.col - 1);
	LIMIT(x2, 0, term.col - 1);
	LIMIT(y1, 0, term.row - 1);
	LIMIT(y2, 0, term.row - 1);

	for (y = y1; y <= y2; y++)
	{
		term.dirty[y] = 1;
		for (x = x1; x <= x2; x++)
		{
			gp = &term.line[y][x];
			if (selected(x, y))
				selclear();
			gp->fg = term.c.attr.fg;
			gp->bg = term.c.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

void
Terminal::tdeletechar(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x;
	src = term.c.x + n;
	size = term.col - src;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(term.col - n, term.c.y, term.col - 1, term.c.y);
}

void
Terminal::tinsertblank(int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, term.col - term.c.x);

	dst = term.c.x + n;
	src = term.c.x;
	size = term.col - dst;
	line = term.line[term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(src, term.c.y, dst - 1, term.c.y);
}

void
Terminal::tinsertblankline(int n)
{
	if (between(term.c.y, term.top, term.bot))
		tscrolldown(term.c.y, n);
}

void
Terminal::tdeleteline(int n)
{
	if (between(term.c.y, term.top, term.bot))
		tscrollup(term.c.y, n);
}

int32_t
Terminal::tdefcolor(const int *attr, int *npar, int l)
{
	int32_t idx = -1;
	uint r, g, b;

	switch (attr[*npar + 1])
	{
		case 2: /* direct color in RGB space */
			if (*npar + 4 >= l)
			{
				fprintf(stderr, "erresc(38): Incorrect number of parameters (%d)\n",
				    *npar);
				break;
			}
			r = attr[*npar + 2];
			g = attr[*npar + 3];
			b = attr[*npar + 4];
			*npar += 4;
			if (!between(r, 0, 255) || !between(g, 0, 255) || !between(b, 0, 255))
				fprintf(stderr, "erresc: bad rgb color (%u,%u,%u)\n", r, g, b);
			else
				idx = TRUECOLOR(r, g, b);
			break;
		case 5: /* indexed color */
			if (*npar + 2 >= l)
			{
				fprintf(stderr, "erresc(38): Incorrect number of parameters (%d)\n",
				    *npar);
				break;
			}
			*npar += 2;
			if (!between(attr[*npar], 0, 255))
				fprintf(stderr, "erresc: bad fgcolor %d\n", attr[*npar]);
			else
				idx = attr[*npar];
			break;
		case 0: /* implemented defined (only foreground) */
		case 1: /* transparent */
		case 3: /* direct color in CMY space */
		case 4: /* direct color in CMYK space */
		default:
			fprintf(stderr, "erresc(38): gfx attr %d unknown\n", attr[*npar]);
			break;
	}

	return idx;
}

void
Terminal::tsetattr(const int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++)
	{
		switch (attr[i])
		{
			case 0:
				term.c.attr.mode &=
				    ~(ATTR_BOLD | ATTR_FAINT | ATTR_ITALIC | ATTR_UNDERLINE |
					ATTR_BLINK | ATTR_REVERSE | ATTR_INVISIBLE | ATTR_STRUCK);
				term.c.attr.fg = defaultfg;
				term.c.attr.bg = defaultbg;
				break;
			case 1:
				term.c.attr.mode |= ATTR_BOLD;
				break;
			case 2:
				term.c.attr.mode |= ATTR_FAINT;
				break;
			case 3:
				term.c.attr.mode |= ATTR_ITALIC;
				break;
			case 4:
				term.c.attr.mode |= ATTR_UNDERLINE;
				break;
			case 5: /* slow blink */
				/* FALLTHROUGH */
			case 6: /* rapid blink */
				term.c.attr.mode |= ATTR_BLINK;
				break;
			case 7:
				term.c.attr.mode |= ATTR_REVERSE;
				break;
			case 8:
				term.c.attr.mode |= ATTR_INVISIBLE;
				break;
			case 9:
				term.c.attr.mode |= ATTR_STRUCK;
				break;
			case 22:
				term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
				break;
			case 23:
				term.c.attr.mode &= ~ATTR_ITALIC;
				break;
			case 24:
				term.c.attr.mode &= ~ATTR_UNDERLINE;
				break;
			case 25:
				term.c.attr.mode &= ~ATTR_BLINK;
				break;
			case 27:
				term.c.attr.mode &= ~ATTR_REVERSE;
				break;
			case 28:
				term.c.attr.mode &= ~ATTR_INVISIBLE;
				break;
			case 29:
				term.c.attr.mode &= ~ATTR_STRUCK;
				break;
			case 38:
				if ((idx = tdefcolor(attr, &i, l)) >= 0)
					term.c.attr.fg = idx;
				break;
			case 39: /* set foreground color to default */
				term.c.attr.fg = defaultfg;
				break;
			case 48:
				if ((idx = tdefcolor(attr, &i, l)) >= 0)
					term.c.attr.bg = idx;
				break;
			case 49: /* set background color to default */
				term.c.attr.bg = defaultbg;
				break;
			case 58:
				/*
			   This starts a sequence to change the color of
			   "underline" pixels. We don't support that and
			   instead eat up a following "5;n" or "2;r;g;b".
			*/
				tdefcolor(attr, &i, l);
				break;
			default:
				if (between(attr[i], 30, 37))
				{
					term.c.attr.fg = attr[i] - 30;
				}
				else if (between(attr[i], 40, 47))
				{
					term.c.attr.bg = attr[i] - 40;
				}
				else if (between(attr[i], 90, 97))
				{
					term.c.attr.fg = attr[i] - 90 + 8;
				}
				else if (between(attr[i], 100, 107))
				{
					term.c.attr.bg = attr[i] - 100 + 8;
				}
				else
				{
					fprintf(stderr, "erresc(default): gfx attr %d unknown\n",
					    attr[i]);
					csidump();
				}
				break;
		}
	}
}

void
Terminal::tsetscroll(int t, int b)
{
	int temp;

	LIMIT(t, 0, term.row - 1);
	LIMIT(b, 0, term.row - 1);
	if (t > b)
	{
		temp = t;
		t = b;
		b = temp;
	}
	term.top = t;
	term.bot = b;
}

void
Terminal::tsetmode(int priv, int set, const int *args, int narg)
{
	int alt;
	const int *lim;

	for (lim = args + narg; args < lim; ++args)
	{
		if (priv)
		{
			switch (*args)
			{
				case 1: /* DECCKM -- Cursor key */
					set_win_mode(set, MODE_APPCURSOR);
					break;
				case 5: /* DECSCNM -- Reverse video */
					set_win_mode(set, MODE_REVERSE);
					break;
				case 6: /* DECOM -- Origin */
					term.c.state =
					    (char) modbit(term.c.state, set, CURSOR_ORIGIN);
					tmoveato(0, 0);
					break;
				case 7: /* DECAWM -- Auto wrap */
					term.mode = modbit(term.mode, set, MODE_WRAP);
					break;
				case 0:	 /* Error (IGNORED) */
				case 2:	 /* DECANM -- ANSI/VT52 (IGNORED) */
				case 3:	 /* DECCOLM -- Column  (IGNORED) */
				case 4:	 /* DECSCLM -- Scroll (IGNORED) */
				case 8:	 /* DECARM -- Auto repeat (IGNORED) */
				case 18: /* DECPFF -- Printer feed (IGNORED) */
				case 19: /* DECPEX -- Printer extent (IGNORED) */
				case 42: /* DECNRCM -- National characters (IGNORED) */
				case 12: /* att610 -- Cursor blink enable/disable */
					cursor_blinking = set;
					cursor_blink_on = true;
					break;
				case 25: /* DECTCEM -- Text Cursor Enable Mode */
					set_win_mode(!set, MODE_HIDE);
					break;
				case 9: /* X10 mouse compatibility mode */
					set_win_mode(0, MODE_MOUSE);
					set_win_mode(set, MODE_MOUSEX10);
					break;
				case 1000: /* 1000: report button press */
					set_win_mode(0, MODE_MOUSE);
					set_win_mode(set, MODE_MOUSEBTN);
					break;
				case 1002: /* 1002: report motion on button press */
					set_win_mode(0, MODE_MOUSE);
					set_win_mode(set, MODE_MOUSEMOTION);
					break;
				case 1003: /* 1003: enable all mouse motions */
					set_win_mode(0, MODE_MOUSE);
					set_win_mode(set, MODE_MOUSEMANY);
					break;
				case 1004: /* 1004: send focus events to tty */
					set_win_mode(set, MODE_FOCUS);
					break;
				case 1006: /* 1006: extended reporting mode */
					set_win_mode(set, MODE_MOUSESGR);
					break;
				case 1034: /* 1034: enable 8-bit mode for keyboard input */
					set_win_mode(set, MODE_8BIT);
					break;
				case 1049: /* swap screen & set/restore cursor as xterm */
					if (!allowaltscreen)
						break;
					tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
					/* FALLTHROUGH */
				case 47:   /* swap screen buffer */
				case 1047: /* swap screen buffer */
					if (!allowaltscreen)
						break;
					alt = IS_SET(MODE_ALTSCREEN);
					if (alt)
					{
						tclearregion(
						    0, 0, term.col - 1, term.row - 1);
					}
					if (set ^ alt) /* set is always 1 or 0 */
						tswapscreen();
					if (*args != 1049)
						break;
					/* FALLTHROUGH */
				case 1048: /* save/restore cursor (like DECSC/DECRC) */
					tcursor((set) ? CURSOR_SAVE : CURSOR_LOAD);
					break;
				case 2004: /* 2004: bracketed paste mode */
					set_win_mode(set, MODE_BRCKTPASTE);
					break;
				// Deliberately unimplemented mouse modes (see case reasons below).
				case 1001: /* mouse highlight mode; can hang the
				      terminal by design when implemented. */
				case 1005: /* UTF-8 mouse mode; will confuse
				      applications not supporting UTF-8
				      and luit. */
				case 1015: /* urxvt mangled mouse mode; incompatible
				      and can be mistaken for other control
				      codes. */
					break;
				default:
					fprintf(stderr,
					    "erresc: unknown private set/reset mode %d\n", *args);
					break;
			}
		}
		else
		{
			switch (*args)
			{
				case 0: /* Error (IGNORED) */
					break;
				case 2:
					set_win_mode(set, MODE_KBDLOCK);
					break;
				case 4: /* IRM -- Insertion-replacement */
					term.mode = modbit(term.mode, set, MODE_INSERT);
					break;
				case 12: /* SRM -- Send/Receive */
					term.mode = modbit(term.mode, !set, MODE_ECHO);
					break;
				case 20: /* LNM -- Linefeed/new line */
					term.mode = modbit(term.mode, set, MODE_CRLF);
					break;
				default:
					fprintf(
					    stderr, "erresc: unknown set/reset mode %d\n", *args);
					break;
			}
		}
	}
}

void
Terminal::csihandle()
{
	char buf[40];
	int len;

	switch (csi.mode[0])
	{
		default:
		unknown:
			fprintf(stderr, "erresc: unknown csi ");
			csidump();
			/* die(""); */
			break;
		case '@': /* ICH -- Insert <n> blank char */
			DEFAULT(csi.arg[0], 1);
			tinsertblank(csi.arg[0]);
			break;
		case 'A': /* CUU -- Cursor <n> Up */
			DEFAULT(csi.arg[0], 1);
			tmoveto(term.c.x, term.c.y - csi.arg[0]);
			break;
		case 'B': /* CUD -- Cursor <n> Down */
		case 'e': /* VPR --Cursor <n> Down */
			DEFAULT(csi.arg[0], 1);
			tmoveto(term.c.x, term.c.y + csi.arg[0]);
			break;
		case 'i': /* MC -- Media Copy */
			switch (csi.arg[0])
			{
				case 0:
					tdump();
					break;
				case 1:
					tdumpline(term.c.y);
					break;
				case 2:
					tdumpsel();
					break;
				case 4:
					term.mode &= ~MODE_PRINT;
					break;
				case 5:
					term.mode |= MODE_PRINT;
					break;
			}
			break;
		case 'c': /* DA -- Device Attributes */
			if (csi.arg[0] == 0)
				ttywrite(vtiden, strlen(vtiden), 0);
			break;
		case 'b': /* REP -- if last char is printable print it <n> more times */
			LIMIT(csi.arg[0], 1, 65535);
			if (term.lastc)
				while (csi.arg[0]-- > 0)
					tputc(term.lastc);
			break;
		case 'C': /* CUF -- Cursor <n> Forward */
		case 'a': /* HPR -- Cursor <n> Forward */
			DEFAULT(csi.arg[0], 1);
			tmoveto(term.c.x + csi.arg[0], term.c.y);
			break;
		case 'D': /* CUB -- Cursor <n> Backward */
			DEFAULT(csi.arg[0], 1);
			tmoveto(term.c.x - csi.arg[0], term.c.y);
			break;
		case 'E': /* CNL -- Cursor <n> Down and first col */
			DEFAULT(csi.arg[0], 1);
			tmoveto(0, term.c.y + csi.arg[0]);
			break;
		case 'F': /* CPL -- Cursor <n> Up and first col */
			DEFAULT(csi.arg[0], 1);
			tmoveto(0, term.c.y - csi.arg[0]);
			break;
		case 'g': /* TBC -- Tabulation clear */
			switch (csi.arg[0])
			{
				case 0: /* clear current tab stop */
					term.tabs[term.c.x] = 0;
					break;
				case 3: /* clear all the tabs */
					memset(
					    term.tabs, 0, term.col * sizeof(*term.tabs));
					break;
				default:
					goto unknown;
			}
			break;
		case 'G': /* CHA -- Move to <col> */
		case '`': /* HPA */
			DEFAULT(csi.arg[0], 1);
			tmoveto(csi.arg[0] - 1, term.c.y);
			break;
		case 'H': /* CUP -- Move to <row> <col> */
		case 'f': /* HVP */
			DEFAULT(csi.arg[0], 1);
			DEFAULT(csi.arg[1], 1);
			tmoveato(csi.arg[1] - 1, csi.arg[0] - 1);
			break;
		case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
			DEFAULT(csi.arg[0], 1);
			tputtab(csi.arg[0]);
			break;
		case 'J': /* ED -- Clear screen */
			switch (csi.arg[0])
			{
				case 0: /* below */
					tclearregion(term.c.x, term.c.y, term.col - 1,
					    term.c.y);
					if (term.c.y < term.row - 1)
					{
						tclearregion(0, term.c.y + 1, term.col - 1,
						    term.row - 1);
					}
					break;
				case 1: /* above */
					if (term.c.y > 0)
						tclearregion(
						    0, 0, term.col - 1, term.c.y - 1);
					tclearregion(0, term.c.y, term.c.x, term.c.y);
					break;
				case 2: /* all */
					tclearregion(0, 0, term.col - 1, term.row - 1);
					break;
				default:
					goto unknown;
			}
			break;
		case 'K': /* EL -- Clear line */
			switch (csi.arg[0])
			{
				case 0: /* right */
					tclearregion(term.c.x, term.c.y, term.col - 1,
					    term.c.y);
					break;
				case 1: /* left */
					tclearregion(0, term.c.y, term.c.x, term.c.y);
					break;
				case 2: /* all */
					tclearregion(
					    0, term.c.y, term.col - 1, term.c.y);
					break;
			}
			break;
		case 'S': /* SU -- Scroll <n> line up */
			if (csi.priv)
				break;
			DEFAULT(csi.arg[0], 1);
			tscrollup(term.top, csi.arg[0]);
			break;
		case 'T': /* SD -- Scroll <n> line down */
			DEFAULT(csi.arg[0], 1);
			tscrolldown(term.top, csi.arg[0]);
			break;
		case 'L': /* IL -- Insert <n> blank lines */
			DEFAULT(csi.arg[0], 1);
			tinsertblankline(csi.arg[0]);
			break;
		case 'l': /* RM -- Reset Mode */
			tsetmode(csi.priv, 0, csi.arg, csi.narg);
			break;
		case 'M': /* DL -- Delete <n> lines */
			DEFAULT(csi.arg[0], 1);
			tdeleteline(csi.arg[0]);
			break;
		case 'X': /* ECH -- Erase <n> char */
			DEFAULT(csi.arg[0], 1);
			tclearregion(term.c.x, term.c.y, term.c.x + csi.arg[0] - 1,
			    term.c.y);
			break;
		case 'P': /* DCH -- Delete <n> char */
			DEFAULT(csi.arg[0], 1);
			tdeletechar(csi.arg[0]);
			break;
		case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
			DEFAULT(csi.arg[0], 1);
			tputtab(-csi.arg[0]);
			break;
		case 'd': /* VPA -- Move to <row> */
			DEFAULT(csi.arg[0], 1);
			tmoveato(term.c.x, csi.arg[0] - 1);
			break;
		case 'h': /* SM -- Set terminal mode */
			tsetmode(csi.priv, 1, csi.arg, csi.narg);
			break;
		case 'm': /* SGR -- Terminal attribute (color) */
			tsetattr(csi.arg, csi.narg);
			break;
		case 'n': /* DSR -- Device Status Report */
			switch (csi.arg[0])
			{
				case 5: /* Status Report "OK" `0n` */
					ttywrite("\033[0n", sizeof("\033[0n") - 1, 0);
					break;
				case 6: /* Report Cursor Position (CPR) "<row>;<column>R" */
					len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
					    term.c.y + 1, term.c.x + 1);
					ttywrite(buf, len, 0);
					break;
				default:
					goto unknown;
			}
			break;
		case 'r': /* DECSTBM -- Set Scrolling Region */
			if (csi.priv)
			{
				goto unknown;
			}
			else
			{
				DEFAULT(csi.arg[0], 1);
				DEFAULT(csi.arg[1], term.row);
				tsetscroll(csi.arg[0] - 1, csi.arg[1] - 1);
				tmoveato(0, 0);
			}
			break;
		case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
			tcursor(CURSOR_SAVE);
			break;
		case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
			if (csi.priv)
			{
				goto unknown;
			}
			else
			{
				tcursor(CURSOR_LOAD);
			}
			break;
		case ' ':
			switch (csi.mode[1])
			{
				case 'q': /* DECSCUSR -- Set Cursor Style */
					if (set_cursor_style(csi.arg[0]))
						goto unknown;
					break;
				default:
					goto unknown;
			}
			break;
	}
}

void
Terminal::csidump()
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < csi.len; i++)
	{
		c = csi.buf[i] & 0xff;
		if (isprint(c))
		{
			putc(c, stderr);
		}
		else if (c == '\n')
		{
			fprintf(stderr, "(\\n)");
		}
		else if (c == '\r')
		{
			fprintf(stderr, "(\\r)");
		}
		else if (c == 0x1b)
		{
			fprintf(stderr, "(\\e)");
		}
		else
		{
			fprintf(stderr, "(%02x)", c);
		}
	}
	putc('\n', stderr);
}

void
Terminal::csireset()
{
	memset(&csi, 0, sizeof(csi));
}

void
Terminal::osc_color_response(int num, int index, int is_osc4)
{
	int n;
	char buf[32];
	unsigned char r, g, b;

	if (get_color(is_osc4 ? num : index, &r, &g, &b))
	{
		fprintf(stderr, "erresc: failed to fetch %s color %d\n", is_osc4 ? "osc4" : "osc",
		    is_osc4 ? num : index);
		return;
	}

	n = snprintf(buf, sizeof buf, "\033]%s%d;rgb:%02x%02x/%02x%02x/%02x%02x\007",
	    is_osc4 ? "4;" : "", num, r, r, g, g, b, b);
	if (n < 0 || n >= sizeof(buf))
	{
		fprintf(stderr, "error: %s while printing %s response\n",
		    n < 0 ? "snprintf failed" : "truncation occurred", is_osc4 ? "osc4" : "osc");
	}
	else
	{
		ttywrite(buf, n, 1);
	}
}

void
Terminal::strhandle()
{
	char *p = NULL, *dec;
	int j, narg, par;
	const struct
	{
		int idx;
		const char *str;
	} osc_table[] = {
	    {(int) defaultfg, "foreground"}, {(int) defaultbg, "background"}, {(int) defaultcs, "cursor"}};

	term.esc &= ~(ESC_STR_END | ESC_STR);
	strparse();
	par = (narg = str.narg) ? atoi(str.args[0]) : 0;

	switch (str.type)
	{
		case ']': /* OSC -- Operating System Command */
			switch (par)
			{
				case 0:
					if (narg > 1)
					{
						set_title(str.args[1]);
					}
					return;
				case 1:
					return;
				case 2:
					if (narg > 1)
						set_title(str.args[1]);
					return;
				case 52: /* manipulate selection data */
					if (narg > 2 && allowwindowops)
					{
						dec = base64dec(str.args[2]);
						if (dec)
						{
							set_clipboard(dec);
							copy_selection();
						}
						else
						{
							fprintf(stderr, "erresc: invalid base64\n");
						}
					}
					return;
				case 10: /* set dynamic VT100 text foreground color */
				case 11: /* set dynamic VT100 text background color */
				case 12: /* set dynamic text cursor color */
					if (narg < 2)
						break;
					p = str.args[1];
					if ((j = par - 10) < 0 || j >= LEN(osc_table))
						break; /* shouldn't be possible */

					if (!strcmp(p, "?"))
					{
						osc_color_response(par, osc_table[j].idx, 0);
					}
					else if (set_color_name(osc_table[j].idx, p))
					{
						fprintf(stderr, "erresc: invalid %s color: %s\n",
						    osc_table[j].str, p);
					}
					else
					{
						tfulldirt();
					}
					return;
				case 4: /* color set */
					if (narg < 3)
						break;
					p = str.args[2];
					/* FALLTHROUGH */
				case 104: /* color reset */
					j = (narg > 1) ? atoi(str.args[1]) : -1;

					if (p && !strcmp(p, "?"))
					{
						osc_color_response(j, 0, 1);
					}
					else if (set_color_name(j, p))
					{
						if (par == 104 && narg <= 1)
						{
							load_colors();
							return; /* color reset without parameter */
						}
						fprintf(stderr,
						    "erresc: invalid color j=%d, p=%s\n", j,
						    p ? p : "(null)");
					}
					else
					{
						/*
				   TODO if defaultbg color is changed, borders
				   are dirty
				*/
						tfulldirt();
					}
					return;
				case 110: /* reset dynamic VT100 text foreground color */
				case 111: /* reset dynamic VT100 text background color */
				case 112: /* reset dynamic text cursor color */
					if (narg != 1)
						break;
					if ((j = par - 110) < 0 || j >= LEN(osc_table))
						break; /* shouldn't be possible */
					if (set_color_name(osc_table[j].idx, NULL))
					{
						fprintf(stderr, "erresc: %s color not found\n",
						    osc_table[j].str);
					}
					else
					{
						tfulldirt();
					}
					return;
			}
			break;
		case 'k': /* old title set compatibility */
			set_title(str.args[0]);
			return;
		case 'P': /* DCS -- Device Control String */
		case '_': /* APC -- Application Program Command */
		case '^': /* PM -- Privacy Message */
			return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump();
}

void
Terminal::strparse()
{
	int c;
	char *p = str.buf;

	str.narg = 0;
	str.buf[str.len] = '\0';

	if (*p == '\0')
		return;

	while (str.narg < STR_ARG_SIZ)
	{
		str.args[str.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
Terminal::strdump()
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC%c", str.type);
	for (i = 0; i < str.len; i++)
	{
		c = str.buf[i] & 0xff;
		if (c == '\0')
		{
			putc('\n', stderr);
			return;
		}
		else if (isprint(c))
		{
			putc(c, stderr);
		}
		else if (c == '\n')
		{
			fprintf(stderr, "(\\n)");
		}
		else if (c == '\r')
		{
			fprintf(stderr, "(\\r)");
		}
		else if (c == 0x1b)
		{
			fprintf(stderr, "(\\e)");
		}
		else
		{
			fprintf(stderr, "(%02x)", c);
		}
	}
	fprintf(stderr, "ESC\\\n");
}

void
Terminal::strreset()
{
	char *strbuf = (char *) xrealloc(str.buf, STR_BUF_SIZ);
	str = STREscape{};
	str.buf = strbuf;
	str.siz = STR_BUF_SIZ;
}

void
Terminal::tprinter(const char *s, size_t len)
{
	if (iofd != -1 && xwrite(iofd, s, len) < 0)
	{
		perror("Error writing to output file");
#ifdef _WIN32
		_close(iofd);
#else
		close(iofd);
#endif
		iofd = -1;
	}
}

void
Terminal::tdumpsel()
{
	char *ptr;

	if ((ptr = getsel()))
	{
		tprinter(ptr, strlen(ptr));
		free(ptr);
	}
}

void
Terminal::tdumpline(int n)
{
	char buf[UTF_SIZ];
	const Glyph *bp, *end;

	bp = &term.line[n][0];
	end = &bp[MIN(tlinelen(n), term.col) - 1];
	if (bp != end || bp->u != ' ')
	{
		for (; bp <= end; ++bp)
			tprinter(buf, utf8encode(bp->u, buf));
	}
	tprinter("\n", 1);
}

void
Terminal::tdump()
{
	int i;

	for (i = 0; i < term.row; ++i)
		tdumpline(i);
}

void
Terminal::tputtab(int n)
{
	uint x = term.c.x;

	if (n > 0)
	{
		while (x < term.col && n--)
			for (++x; x < term.col && !term.tabs[x]; ++x)
				/* nothing */;
	}
	else if (n < 0)
	{
		while (x > 0 && n++)
			for (--x; x > 0 && !term.tabs[x]; --x)
				/* nothing */;
	}
	term.c.x = LIMIT(x, 0, term.col - 1);
}

void
Terminal::tdefutf8(char ascii)
{
	if (ascii == 'G')
		term.mode |= MODE_UTF8;
	else if (ascii == '@')
		term.mode &= ~MODE_UTF8;
}

void
Terminal::tdeftran(char ascii)
{
	static char cs[] = "0B";
	static int vcs[] = {CS_GRAPHIC0, CS_USA};
	char *p;

	if ((p = strchr(cs, ascii)) == NULL)
	{
		fprintf(stderr, "esc unhandled charset: ESC ( %c\n", ascii);
	}
	else
	{
		term.trantbl[term.icharset] = vcs[p - cs];
	}
}

void
Terminal::tdectest(char c)
{
	int x, y;

	if (c == '8')
	{ /* DEC screen alignment test. */
		for (x = 0; x < term.col; ++x)
		{
			for (y = 0; y < term.row; ++y)
				tsetchar('E', &term.c.attr, x, y);
		}
	}
}

void
Terminal::tstrsequence(uchar c)
{
	switch (c)
	{
		case 0x90: /* DCS -- Device Control String */
			c = 'P';
			break;
		case 0x9f: /* APC -- Application Program Command */
			c = '_';
			break;
		case 0x9e: /* PM -- Privacy Message */
			c = '^';
			break;
		case 0x9d: /* OSC -- Operating System Command */
			c = ']';
			break;
	}
	strreset();
	str.type = c;
	term.esc |= ESC_STR;
}

void
Terminal::tcontrolcode(uchar ascii)
{
	switch (ascii)
	{
		case '\t': /* HT */
			tputtab(1);
			return;
		case '\b': /* BS */
			tmoveto(term.c.x - 1, term.c.y);
			return;
		case '\r': /* CR */
			tmoveto(0, term.c.y);
			return;
		case '\f': /* LF */
		case '\v': /* VT */
		case '\n': /* LF */
			// go to first col if the mode is set
			tnewline(IS_SET(MODE_CRLF));
			return;
		case '\a': /* BEL */
			if (term.esc & ESC_STR_END)
			{
				// backwards compatibility to xterm
				strhandle();
			}
			break;
		case '\033': /* ESC */
			csireset();
			term.esc &= ~(ESC_CSI | ESC_ALTCHARSET | ESC_TEST);
			term.esc |= ESC_START;
			return;
		case '\016': /* SO (LS1 -- Locking shift 1) */
		case '\017': /* SI (LS0 -- Locking shift 0) */
			term.charset = 1 - (ascii - '\016');
			return;
		case '\032': /* SUB */
			tsetchar('?', &term.c.attr, term.c.x, term.c.y);
			/* FALLTHROUGH */
		case '\030': /* CAN */
			csireset();
			break;
		case '\005': /* ENQ (IGNORED) */
		case '\000': /* NUL (IGNORED) */
		case '\021': /* XON (IGNORED) */
		case '\023': /* XOFF (IGNORED) */
		case 0177:   /* DEL (IGNORED) */
			return;
		case 0x80: /* TODO: PAD */
		case 0x81: /* TODO: HOP */
		case 0x82: /* TODO: BPH */
		case 0x83: /* TODO: NBH */
		case 0x84: /* TODO: IND */
			break;
		case 0x85:		/* NEL -- Next line */
			tnewline(1); /* always go to first col */
			break;
		case 0x86: /* TODO: SSA */
		case 0x87: /* TODO: ESA */
			break;
		case 0x88: /* HTS -- Horizontal tab stop */
			term.tabs[term.c.x] = 1;
			break;
		case 0x89: /* TODO: HTJ */
		case 0x8a: /* TODO: VTS */
		case 0x8b: /* TODO: PLD */
		case 0x8c: /* TODO: PLU */
		case 0x8d: /* TODO: RI */
		case 0x8e: /* TODO: SS2 */
		case 0x8f: /* TODO: SS3 */
		case 0x91: /* TODO: PU1 */
		case 0x92: /* TODO: PU2 */
		case 0x93: /* TODO: STS */
		case 0x94: /* TODO: CCH */
		case 0x95: /* TODO: MW */
		case 0x96: /* TODO: SPA */
		case 0x97: /* TODO: EPA */
		case 0x98: /* TODO: SOS */
		case 0x99: /* TODO: SGCI */
			break;
		case 0x9a: /* DECID -- Identify Terminal */
			ttywrite(vtiden, strlen(vtiden), 0);
			break;
		case 0x9b: /* TODO: CSI */
		case 0x9c: /* TODO: ST */
			break;
		case 0x90: /* DCS -- Device Control String */
		case 0x9d: /* OSC -- Operating System Command */
		case 0x9e: /* PM -- Privacy Message */
		case 0x9f: /* APC -- Application Program Command */
			tstrsequence(ascii);
			return;
	}
	// only CAN, SUB, \a and C1 chars interrupt a sequence
	term.esc &= ~(ESC_STR_END | ESC_STR);
}

/*
   returns 1 when the sequence is finished and it hasn't to read
   more characters for this sequence, otherwise 0
*/
int
Terminal::eschandle(uchar ascii)
{
	switch (ascii)
	{
		case '[':
			term.esc |= ESC_CSI;
			return 0;
		case '#':
			term.esc |= ESC_TEST;
			return 0;
		case '%':
			term.esc |= ESC_UTF8;
			return 0;
		case 'P': /* DCS -- Device Control String */
		case '_': /* APC -- Application Program Command */
		case '^': /* PM -- Privacy Message */
		case ']': /* OSC -- Operating System Command */
		case 'k': /* old title set compatibility */
			tstrsequence(ascii);
			return 0;
		case 'n': /* LS2 -- Locking shift 2 */
		case 'o': /* LS3 -- Locking shift 3 */
			term.charset = 2 + (ascii - 'n');
			break;
		case '(': /* GZD4 -- set primary charset G0 */
		case ')': /* G1D4 -- set secondary charset G1 */
		case '*': /* G2D4 -- set tertiary charset G2 */
		case '+': /* G3D4 -- set quaternary charset G3 */
			term.icharset = ascii - '(';
			term.esc |= ESC_ALTCHARSET;
			return 0;
		case 'D': /* IND -- Linefeed */
			if (term.c.y == term.bot)
			{
				tscrollup(term.top, 1);
			}
			else
			{
				tmoveto(term.c.x, term.c.y + 1);
			}
			break;
		case 'E':		/* NEL -- Next line */
			tnewline(1); /* always go to first col */
			break;
		case 'H': /* HTS -- Horizontal tab stop */
			term.tabs[term.c.x] = 1;
			break;
		case 'M': /* RI -- Reverse index */
			if (term.c.y == term.top)
			{
				tscrolldown(term.top, 1);
			}
			else
			{
				tmoveto(term.c.x, term.c.y - 1);
			}
			break;
		case 'Z': /* DECID -- Identify Terminal */
			ttywrite(vtiden, strlen(vtiden), 0);
			break;
		case 'c': /* RIS -- Reset to initial state */
			treset();
			resettitle();
			load_colors();
			set_win_mode(0, MODE_HIDE);
			set_win_mode(0, MODE_BRCKTPASTE);
			break;
		case '=': /* DECPAM -- Application keypad */
			set_win_mode(1, MODE_APPKEYPAD);
			break;
		case '>': /* DECPNM -- Normal keypad */
			set_win_mode(0, MODE_APPKEYPAD);
			break;
		case '7': /* DECSC -- Save Cursor */
			tcursor(CURSOR_SAVE);
			break;
		case '8': /* DECRC -- Restore Cursor */
			tcursor(CURSOR_LOAD);
			break;
		case '\\': /* ST -- String Terminator */
			if (term.esc & ESC_STR_END)
				strhandle();
			break;
		default:
			fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n", (uchar) ascii,
			    isprint(ascii) ? ascii : '.');
			break;
	}
	return 1;
}

void
Terminal::tputc(Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127 || !IS_SET(MODE_UTF8))
	{
		c[0] = u;
		width = len = 1;
	}
	else
	{
		len = utf8encode(u, c);
		if (!control && (width = wcwidth(u)) == -1)
			width = 1;
	}

	if (IS_SET(MODE_PRINT))
		tprinter(c, len);

	/*
	   STR sequence must be checked before anything else
	   because it uses all following characters until it
	   receives a ESC, a SUB, a ST or any other C1 control
	   character.
	*/
	if (term.esc & ESC_STR)
	{
		if (u == '\a' || u == 030 || u == 032 || u == 033 || ISCONTROLC1(u))
		{
			term.esc &= ~(ESC_START | ESC_STR);
			term.esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (str.len + len >= str.siz)
		{
			/*
			   Here is a bug in terminals. If the user never sends
			   some code to stop the str or esc command, then st
			   will stop responding. But this is better than
			   silently failing with unknown characters. At least
			   then users will report back.

			   In the case users ever get fixed, here is the code:
			*/
			/*
			 * term.esc = 0;
			 * strhandle();
			 */
			if (str.siz > (SIZE_MAX - UTF_SIZ) / 2)
				return;
			str.siz *= 2;
			str.buf = (char *) xrealloc(str.buf, str.siz);
		}

		memmove(&str.buf[str.len], c, len);
		str.len += len;
		return;
	}

check_control_code:
	/*
	   Actions of control codes must be performed as soon they arrive
	   because they can be embedded inside a control sequence, and
	   they must not cause conflicts with sequences.
	*/
	if (control)
	{
		// in UTF-8 mode ignore handling C1 control characters
		if (IS_SET(MODE_UTF8) && ISCONTROLC1(u))
			return;
		tcontrolcode(u);
		/*
		   control codes are not shown ever
		*/
		if (!term.esc)
			term.lastc = 0;
		return;
	}
	else if (term.esc & ESC_START)
	{
		if (term.esc & ESC_CSI)
		{
			csi.buf[csi.len++] = u;
			if (between(u, 0x40, 0x7E) || csi.len >= sizeof(csi.buf) - 1)
			{
				term.esc = 0;
				csiparse();
				csihandle();
			}
			return;
		}
		else if (term.esc & ESC_UTF8)
		{
			tdefutf8(u);
		}
		else if (term.esc & ESC_ALTCHARSET)
		{
			tdeftran(u);
		}
		else if (term.esc & ESC_TEST)
		{
			tdectest(u);
		}
		else
		{
			if (!eschandle(u))
				return;
			// sequence already finished
		}
		term.esc = 0;
		return;
	}
	if (selected(term.c.x, term.c.y))
		selclear();

	gp = &term.line[term.c.y][term.c.x];
	if (IS_SET(MODE_WRAP) && (term.c.state & CURSOR_WRAPNEXT))
	{
		gp->mode |= ATTR_WRAP;
		tnewline(1);
		gp = &term.line[term.c.y][term.c.x];
	}

	if (IS_SET(MODE_INSERT) && term.c.x + width < term.col)
	{
		memmove(gp + width, gp, (term.col - term.c.x - width) * sizeof(Glyph));
		gp->mode &= ~ATTR_WIDE;
	}

	if (term.c.x + width > term.col)
	{
		if (IS_SET(MODE_WRAP))
			tnewline(1);
		else
			tmoveto(term.col - width, term.c.y);
		gp = &term.line[term.c.y][term.c.x];
	}

	tsetchar(u, &term.c.attr, term.c.x, term.c.y);
	term.lastc = u;

	if (width == 2)
	{
		gp->mode |= ATTR_WIDE;
		if (term.c.x + 1 < term.col)
		{
			if (gp[1].mode == ATTR_WIDE && term.c.x + 2 < term.col)
			{
				gp[2].u = ' ';
				gp[2].mode &= ~ATTR_WDUMMY;
			}
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (term.c.x + width < term.col)
	{
		tmoveto(term.c.x + width, term.c.y);
	}
	else
	{
		term.c.state |= CURSOR_WRAPNEXT;
	}
}

int
Terminal::twrite(const char *buf, int buflen, int show_ctrl)
{
	int charsize;
	Rune u;
	int n;

	for (n = 0; n < buflen; n += charsize)
	{
		if (IS_SET(MODE_UTF8))
		{
			// process a complete utf8 char
			charsize = utf8decode(buf + n, &u, buflen - n);
			if (charsize == 0)
				break;
		}
		else
		{
			u = buf[n] & 0xFF;
			charsize = 1;
		}
		if (show_ctrl && ISCONTROL(u))
		{
			if (u & 0x80)
			{
				u &= 0x7f;
				tputc('^');
				tputc('[');
			}
			else if (u != '\n' && u != '\r' && u != '\t')
			{
				u ^= 0x40;
				tputc('^');
			}
		}
		tputc(u);
	}
	return n;
}

void
Terminal::tresize(int col, int row)
{
	int i;
	int minrow = MIN(row, term.row);
	int mincol = MIN(col, term.col);
	int *bp;
	TCursor c;

	if (col < 1 || row < 1)
	{
		fprintf(stderr, "tresize: error resizing to %dx%d\n", col, row);
		return;
	}

	/*
	   slide screen to keep cursor where we expect it -
	   tscrollup would work here, but we can optimize to
	   memmove because we're freeing the earlier lines
	*/
	for (i = 0; i <= term.c.y - row; i++)
	{
		free(term.line[i]);
		free(term.alt[i]);
	}
	// ensure that both src and dst are not NULL
	if (i > 0)
	{
		memmove(term.line, term.line + i, row * sizeof(Line));
		memmove(term.alt, term.alt + i, row * sizeof(Line));
	}
	for (i += row; i < term.row; i++)
	{
		free(term.line[i]);
		free(term.alt[i]);
	}

	// resize to new height
	term.line = (Line *) xrealloc(term.line, row * sizeof(Line));
	term.alt = (Line *) xrealloc(term.alt, row * sizeof(Line));
	term.dirty = (int *) xrealloc(term.dirty, row * sizeof(*term.dirty));
	term.tabs = (int *) xrealloc(term.tabs, col * sizeof(*term.tabs));

	// resize each row to new width, zero-pad if needed
	for (i = 0; i < minrow; i++)
	{
		term.line[i] = (Line) xrealloc(term.line[i], col * sizeof(Glyph));
		term.alt[i] = (Line) xrealloc(term.alt[i], col * sizeof(Glyph));
	}

	// allocate any new rows
	for (/* i = minrow */; i < row; i++)
	{
		term.line[i] = (Line) xmalloc(col * sizeof(Glyph));
		term.alt[i] = (Line) xmalloc(col * sizeof(Glyph));
	}
	if (col > term.col)
	{
		bp = term.tabs + term.col;

		memset(bp, 0, sizeof(*term.tabs) * (col - term.col));
		while (--bp > term.tabs && !*bp)
			/* nothing */;
		for (bp += tabspaces; bp < term.tabs + col; bp += tabspaces)
			*bp = 1;
	}
	// update terminal size
	term.col = col;
	term.row = row;
	// reset scrolling region
	tsetscroll(0, row - 1);
	// make use of the LIMIT in tmoveto
	tmoveto(term.c.x, term.c.y);
	// Clearing both screens (it makes dirty all lines)
	c = term.c;
	for (i = 0; i < 2; i++)
	{
		if (mincol < col && 0 < minrow)
		{
			tclearregion(mincol, 0, col - 1, minrow - 1);
		}
		if (0 < col && minrow < row)
		{
			tclearregion(0, minrow, col - 1, row - 1);
		}
		tswapscreen();
		tcursor(CURSOR_LOAD);
	}
	term.c = c;
}

void
Terminal::resettitle()
{
	set_title(NULL);
}

void
Terminal::drawregion(int x1, int y1, int x2, int y2)
{
	int y;

	for (y = y1; y < y2; y++)
	{
		if (!term.dirty[y])
			continue;

		term.dirty[y] = 0;
		draw_line(term.line[y], x1, y, x2);
	}
}

void
Terminal::draw()
{
	int cx = term.c.x;

	if (!begin_draw())
		return;

	// adjust cursor position
	LIMIT(term.ocx, 0, term.col - 1);
	LIMIT(term.ocy, 0, term.row - 1);
	if (term.line[term.ocy][term.ocx].mode & ATTR_WDUMMY)
		term.ocx--;
	if (term.line[term.c.y][cx].mode & ATTR_WDUMMY)
		cx--;

	drawregion(0, 0, term.col, term.row);
	draw_cursor(cx, term.c.y, term.line[term.c.y][cx], term.ocx, term.ocy,
	    term.line[term.ocy][term.ocx]);
	term.ocx = cx;
	term.ocy = term.c.y;
}

void
Terminal::redraw()
{
	tfulldirt();
	draw();
}

/* ----------------------------------------------------------------------
    Emulator config globals — shared defaults read by the core.
   ---------------------------------------------------------------------- */

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

// ---- Terminal output operations (driven by the emulator core) ----

void
Terminal::copy_selection()
{
	char *sel = getsel();
	if (sel)
	{
		ImGui::SetClipboardText(sel);
		free(sel);
	}
}

void
Terminal::handle_child_exit(const char *msg)
{
	/* A fatal error confined to this terminal: surface it without taking
	   down the process. The emulator marks alive = 0 right after this
	   returns, so is_alive() will report the terminal as dead. */
	fprintf(stderr, "imgui_terminal: %s", msg ? msg : "fatal error\n");
	snprintf(title, sizeof title, "[exited] %s", msg ? msg : "");
	title[sizeof title - 1] = '\0';
}

void
Terminal::draw_cursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og)
{
	(void) ox;
	(void) oy;
	(void) og;
	emit_target = &overlay_ops;

	if (tw.mode & MODE_HIDE)
		return;
	if ((tw.mode & MODE_FOCUSED) && cursor_blinking && !cursor_blink_on)
		return;

	g.mode &= ATTR_BOLD | ATTR_ITALIC | ATTR_UNDERLINE | ATTR_STRUCK | ATTR_WIDE;

	ImU32 drawcol;
	if (tw.mode & MODE_REVERSE)
	{
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (selected(cx, cy))
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
		if (selected(cx, cy))
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
Terminal::draw_line(Line line, int x1, int y, int x2)
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
		if (selected(x, y))
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
Terminal::load_colors()
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
Terminal::set_color_name(int x, const char *name)
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
Terminal::get_color(int x, unsigned char *r, unsigned char *g, unsigned char *b)
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
Terminal::set_title(char *p)
{
	snprintf(title, sizeof title, "%s", p ? p : "Terminal");
}

int
Terminal::set_cursor_style(int cursor)
{
	if (!between(cursor, 0, 7))
		return 1;
	tw.cursor = cursor;
	/* DECSCUSR: 0/1/3/5 = blinking, 2/4/6/7 = steady */
	cursor_blinking = (cursor <= 1 || cursor == 3 || cursor == 5);
	cursor_blink_on = true;
	return 0;
}

void
Terminal::set_win_mode(int set, unsigned int flags)
{
	int prev = tw.mode;
	tw.mode = modbit(tw.mode, set, flags);
	//  Full redraw on whole-screen-reverse-video flip.
	if ((tw.mode & MODE_REVERSE) != (prev & MODE_REVERSE))
		redraw();

	if ((tw.mode & MODE_MOUSE) != (prev & MODE_MOUSE))
	{
		selecting = false;
		last_motion_x = -1;
		last_motion_y = -1;
	}
}

void
Terminal::set_clipboard(char *str)
{
	if (str)
		ImGui::SetClipboardText(str);
}

int
Terminal::begin_draw()
{
	overlay_ops.clear();
	return (tw.mode & MODE_VISIBLE) != 0;
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

	/* Text blink (ATTR_BLINK) */
	if (now_ms - blink_last_toggle_ms >= (double) s_blinktimeout)
	{
		if (tattrset(ATTR_BLINK))
		{
			tw.mode ^= MODE_BLINK;
			tsetdirtattr(ATTR_BLINK);
			m_changed = true;
		}
		else
		{
			tw.mode |= MODE_BLINK;
		}
		blink_last_toggle_ms = now_ms;
	}

	/* Cursor blink */
	if (cursor_blinking &&
	    now_ms - cursor_blink_timer >= (double) s_cursorblinktimeout)
	{
		cursor_blink_on = !cursor_blink_on;
		cursor_blink_timer = now_ms;
		m_changed = true;
	}
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
	ttywrite(buf, 6, 0);
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
		ttywrite(buf, (size_t) n, 0);
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
	if (cmdfd < 0)
		return;

	const double drain_budget_s = 0.005;
	const double deadline = ImGui::GetTime() + drain_budget_s;
#ifdef _WIN32
	for (;;)
	{
		DWORD avail = 0;
		if (!PeekNamedPipe(w32_pipe_in, NULL, 0, NULL, &avail, NULL))
			break;
		if (avail == 0)
			break;
		ttyread();
		if (!alive)
			break;
		m_changed = true;
		if (ImGui::GetTime() >= deadline)
			break;
	}
#else
	fd_set rfds;
	struct timeval tv = {0, 0};
	for (;;)
	{
		FD_ZERO(&rfds);
		FD_SET(cmdfd, &rfds);
		int n = select(cmdfd + 1, &rfds, NULL, NULL, &tv);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			break;
		}
		if (n == 0 || !FD_ISSET(cmdfd, &rfds))
			break;
		ttyread();
		if (!alive)
			break;
		m_changed = true;
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
		/* Non-ASCII characters always use the base font — it has
		   the merged CJK / emoji / symbol fallbacks.  Bold / italic
		   variants are only meaningful for Latin text anyway. */
		if (rune < 0x80)
		{
			if ((mode & ATTR_BOLD_FAINT) == ATTR_BOLD && (mode & ATTR_ITALIC))
				fnt = &s_dc.ibfont;
			else if ((mode & ATTR_BOLD_FAINT) == ATTR_BOLD)
				fnt = &s_dc.bfont;
			else if (mode & ATTR_ITALIC)
				fnt = &s_dc.ifont;
		}

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
	if (!pat) return;
	FcConfigSubstitute(NULL, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);

	FcResult result;
	FcPattern *match = FcFontMatch(NULL, pat, &result);
	FcPatternDestroy(pat);
	if (!match) return;

	FcChar8 *file = NULL;
	if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file)
	{
		FcPatternDestroy(match);
		return;
	}

	ImFontConfig cfg;
	cfg.MergeMode = true;
	cfg.FontLoaderFlags = ImGuiFreeTypeLoaderFlags_LoadColor | ImGuiFreeTypeLoaderFlags_Bitmap;
#ifndef _WIN32
	if (strstr(fc_query, "und-zsye") != NULL)
		cfg.RasterizerDensity = 20.0f / (float) pxsize;
#endif
	ImGui::GetIO().Fonts->AddFontFromFileTTF((const char *) file, (float) pxsize, &cfg);
	FcPatternDestroy(match);
}

static const ImWchar s_terminal_symbol_ranges[] = {
    0x2190, 0x21FF, /* Arrows */
    0x2500, 0x257F, /* Box Drawing */
    0x2580, 0x259F, /* Block Elements */
    0x25A0, 0x25FF, /* Geometric Shapes */
    0x2600, 0x26FF, /* Misc Symbols */
    0x2700, 0x27BF, /* Dingbats */
    0x2800, 0x28FF, /* Braille Patterns */
    0,
};

static void
imw_load_terminal_symbols(double pxsize)
{
	static const char *queries[] = {
		":family=Apple Symbols",
		":family=Symbol",
		":charset=2800",  /* braille — narrowest query, matches best font */
		":charset=2500",  /* box drawing */
		NULL,
	};

	ImFontConfig cfg;
	cfg.MergeMode = true;
	cfg.GlyphRanges = s_terminal_symbol_ranges;

	for (int i = 0; queries[i]; i++)
	{
		FcPattern *pat = FcNameParse((const FcChar8 *) queries[i]);
		if (!pat) continue;
		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);

		FcResult result;
		FcPattern *match = FcFontMatch(NULL, pat, &result);
		FcPatternDestroy(pat);
		if (!match) continue;

		FcChar8 *file = NULL;
		bool ok = (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file);
		if (ok)
			ImGui::GetIO().Fonts->AddFontFromFileTTF(
			    (const char *) file, (float) pxsize, &cfg);
		FcPatternDestroy(match);
		if (ok)
			return;
	}
}

void
Terminal::load_fonts_once()
{
	if (s_fonts_loaded)
		return;

	/* Deterministic font for the test harness. When TERMINAL_FONT points at a
	   .ttf we load it directly (bypassing fontconfig) into all four slots, so
	   cell metrics and variant routing are identical on every machine — the
	   snapshot tests can't depend on whatever fonts the host happens to have.
	   The product path below uses normal system font discovery. */
	if (const char *test_font = getenv("TERMINAL_FONT"); test_font && *test_font)
	{
		ImGui::GetIO().Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
		Font *slots[4] = {&s_dc.font, &s_dc.ifont, &s_dc.ibfont, &s_dc.bfont};
		for (Font *f : slots)
		{
			memset(f, 0, sizeof *f);
			f->pxsize = 16.0f;
			ImFontConfig cfg;
			f->match =
			    ImGui::GetIO().Fonts->AddFontFromFileTTF(test_font, 16.0f, &cfg);
			if (!f->match)
				die("imgui_terminal: cannot load TERMINAL_FONT: %s\n", test_font);
		}
		s_fonts_loaded = true;
		return;
	}
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

	/* Load the base font first — it gets the full glyph set. */
	imw_load_one_font(&s_dc.font, pattern, pxsize);
	imw_load_terminal_symbols(pxsize);
	imw_load_fallback(":lang=ja", pxsize);
	imw_load_fallback(":lang=und-zsye", pxsize);

	/* Variant fonts only need ASCII/Latin — non-ASCII glyphs always
	   render through the base font (see makeglyphfontspecs). */
	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	imw_load_one_font(&s_dc.ifont, pattern, pxsize);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	imw_load_one_font(&s_dc.ibfont, pattern, pxsize);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	imw_load_one_font(&s_dc.bfont, pattern, pxsize);

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
	tresize(new_cols, new_rows);

	ttyresize(tw.tw, tw.th);

	row_ops.resize(new_rows);

	redraw();
	m_changed = true;
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
			ttywrite(kp->s, strlen(kp->s), 1);
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
				ttywrite(&byte, 1, 1);
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
				ttywrite(&ctrl_c0_map[i].byte, 1, 1);
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
			ttywrite(buf, (size_t) n, 1);
		}
	}
	if (consumed_this_frame)
		m_changed = true;
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
		selstart(col, row, snap);
		selecting = true;
	}
	if (selecting && ImGui::IsMouseDragging(ImGuiMouseButton_Left))
	{
		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		pixel_to_cell(mpos, &col, &row);
		selextend(col, row, drag_sel_type, 0);
	}
	if (selecting && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		ImVec2 mpos = ImGui::GetMousePos();
		int col, row;
		pixel_to_cell(mpos, &col, &row);
		selextend(col, row, drag_sel_type, 1);
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

	for (int b = 0; b < 5; b++)
	{
		if (io.MouseDown[b])
		{
			m_changed = true;
			break;
		}
	}
}

/* ----------------------------------------------------------------------
   Public widget API
   ---------------------------------------------------------------------- */

bool
Terminal::is_alive() const
{
	return alive != 0;
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
	m_changed = true;
}

void
Terminal::set_retained(bool on)
{
	m_retained = on;
}

void
Terminal::init(int cols, int rows, char **argv)
{

	tw.cw = 8;
	tw.ch = 16;
	tw.tw = cols * tw.cw;
	tw.th = rows * tw.ch;
	tw.w = tw.tw;
	tw.h = tw.th;
	tw.mode = MODE_VISIBLE | MODE_FOCUSED | MODE_NUMLOCK;
	tw.cursor = 1;
	cursor_blinking = false;
	cursor_blink_timer = ImGui::GetTime() * 1000.0;

	load_fonts_once();
	alive = 1;
	load_colors();
	row_ops.resize(rows);
#ifndef _WIN32
	setenv("TERM_PROGRAM", "st-imgui", 1);
#endif
	tnew(cols, rows);
	selinit();

	static char zsh_name[] = "zsh";
	char *shell_argv[] = {zsh_name, (char *) "+o", (char *) "PROMPT_SP", NULL};
	char **child_argv = argv;
	if (!argv)
	{
		const char *sh = getenv("SHELL");
		if (sh && strstr(sh, "zsh"))
			child_argv = shell_argv;
	}
	int fd = ttynew(NULL, NULL, NULL, child_argv);
	if (fd < 0)
	{
		/* ttynew already reported the failure via handle_child_exit and marked the
		   emulator dead. Don't abort the whole app — this terminal simply
		   comes up inert and is_alive() returns false. */
		alive = 0;
	}
}

bool
Terminal::draw_canvas()
{
	if (!alive)
		return false;

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
		ttywrite(focused ? "\033[I" : "\033[O", 3, 0);
	if (focused != was_focused)
		m_changed = true;
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
		draw();

	bool changed = m_changed;
	if (changed || !m_retained)
	{
		for (size_t y = 0; y < row_ops.size(); y++)
			replay_ops(row_ops[y], canvas_pos, dl);
		replay_ops(overlay_ops, canvas_pos, dl);
	}
	m_changed = false;
	return changed;
}

bool
Terminal::draw_widget(const char *id)
{
	if (!alive)
		return false;

	// Title with persistent ID.
	char label[300];
	snprintf(label, sizeof label, "%s%s", title, id ? id : "###term_widget");

	ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);

	if (ImGui::Begin(label))
		draw_canvas();
	ImGui::End();
	return true;
}

void
Terminal::shutdown()
{
	if (cmdfd >= 0)
	{
		ttyhangup();
		cmdfd = -1;
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
	/*  Force cursor visible during dump so tests get deterministic
	    snapshots regardless of blink phase. */
	bool saved_blinking = cursor_blinking;
	cursor_blinking = false;
	cursor_blink_on = true;

	/*  Force a full redraw so the per-row cache reflects the
	    post-processing state of Term, not whatever was cached partway
	    through. Tests rely on this. */
	redraw();

	cursor_blinking = saved_blinking;

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

DC Terminal::s_dc;
bool Terminal::s_fonts_loaded = false;

void
Terminal::s_clipcopy(Terminal *t, const Arg *)
{
	t->copy_selection();
}
void
Terminal::s_clippaste(Terminal *t, const Arg *)
{
	const char *txt = ImGui::GetClipboardText();
	if (!txt || !*txt)
		return;
	if (t->cursor_blinking)
	{
		t->cursor_blink_on = true;
		t->cursor_blink_timer = ImGui::GetTime() * 1000.0;
	}
	size_t n = strlen(txt);
	if (t->tw.mode & MODE_BRCKTPASTE)
		t->ttywrite("\033[200~", 6, 0);
	t->ttywrite(txt, n, 0);
	if (t->tw.mode & MODE_BRCKTPASTE)
		t->ttywrite("\033[201~", 6, 0);
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
		t->ttywrite(a->s, strlen(a->s), 1);
}
