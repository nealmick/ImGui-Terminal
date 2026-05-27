/* See LICENSE for license details. */

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

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN /* skip legacy headers + macro pollution */
#include <windows.h>
#include <process.h>
#include <io.h>
#include <sys/types.h> /* pid_t (only used in POSIX paths) */
#ifdef _MSC_VER
/* MSVC's <sys/types.h> doesn't define ssize_t. SSIZE_T (uppercase)
   comes from <BaseTsd.h>, included transitively by <windows.h>. */
typedef SSIZE_T ssize_t;
#endif
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

#include "emulator.h"
#include "callbacks.h"

#ifndef _WIN32
#if defined(__linux)
#include <pty.h>
#elif defined(__OpenBSD__) || defined(__NetBSD__) || defined(__APPLE__)
#include <util.h>
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <libutil.h>
#endif
#endif

/* Emulator-only macros. */

#define LEN(a) (sizeof(a) / sizeof(a[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) < (b) ? (b) : (a))
#define DIVCEIL(n, d) (((n) + ((d) - 1)) / (d))
#define DEFAULT(a, b) (a) = (a) ? (a) : (b)
#define LIMIT(x, a, b) (x) = (x)<(a) ? (a) : (x)>(b) ? (b) : (x)
#define TIMEDIFF(t1, t2) ((t1.tv_sec - t2.tv_sec) * 1000 + (t1.tv_nsec - t2.tv_nsec) / 1E6)
#define TRUECOLOR(r, g, b) (1 << 24 | (r) << 16 | (g) << 8 | (b))

#define IS_SET(e, flag) (((e)->term.mode & (flag)) != 0)

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

static void
ttywriteraw(Emulator *e, const char *, size_t);
static void
csidump(Emulator *e);
static void
csihandle(Emulator *e);
static void
csiparse(Emulator *e);
static void
csireset(Emulator *e);
static void
osc_color_response(Emulator *e, int, int, int);
static int
eschandle(Emulator *e, uchar);
static void
strdump(Emulator *e);
static void
strhandle(Emulator *e);
static void
strparse(Emulator *e);
static void
strreset(Emulator *e);
static void
tprinter(Emulator *e, char *, size_t);
static void
tdumpsel(Emulator *e);
static void
tdumpline(Emulator *e, int);
static void
tdump(Emulator *e);
static void
tclearregion(Emulator *e, int, int, int, int);
static void
tcursor(Emulator *e, int);
static void
tdeletechar(Emulator *e, int);
static void
tdeleteline(Emulator *e, int);
static void
tinsertblank(Emulator *e, int);
static void
tinsertblankline(Emulator *e, int);
static int
tlinelen(Emulator *e, int);
static void
tmoveto(Emulator *e, int, int);
static void
tmoveato(Emulator *e, int, int);
static void
tnewline(Emulator *e, int);
static void
tputtab(Emulator *e, int);
static void
tputc(Emulator *e, Rune);
static void
treset(Emulator *e);
static void
tscrollup(Emulator *e, int, int);
static void
tscrolldown(Emulator *e, int, int);
static void
tsetattr(Emulator *e, const int *, int);
static void
tsetchar(Emulator *e, Rune, const Glyph *, int, int);
static void
tsetdirt(Emulator *e, int, int);
static void
tsetscroll(Emulator *e, int, int);
static void
tswapscreen(Emulator *e);
static void
tsetmode(Emulator *e, int, int, const int *, int);
static int
twrite(Emulator *e, const char *, int, int);
static void
tfulldirt(Emulator *e);
static void
tcontrolcode(Emulator *e, uchar);
static void
tdectest(Emulator *e, char);
static void
tdefutf8(Emulator *e, char);
static int32_t
tdefcolor(Emulator *e, const int *, int *, int);
static void
tdeftran(Emulator *e, char);
static void
tstrsequence(Emulator *e, uchar);

static void
drawregion(Emulator *e, int, int, int, int);

static void
selnormalize(Emulator *e);
static void
selscroll(Emulator *e, int, int);
static void
selsnap(Emulator *e, int *, int *, int);

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

/* Per-instance state lives on Emulator (see emulator.h). UTF tables below
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
		[43] = 62, 0, 0, 0, 63, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 0, 0, 0,
		-1, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
		17, 18, 19, 20, 21, 22, 23, 24, 25, 0, 0, 0, 0, 0, 0, 26, 27, 28, 29, 30,
		31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
		49, 50, 51
	};
	/* clang-format on */

	if (in_len % 4)
		in_len += 4 - (in_len % 4);
	result = dst = xmalloc(in_len / 4 * 3 + 1);
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
selinit(Emulator *e)
{
	e->sel.mode = SEL_IDLE;
	e->sel.snap = 0;
	e->sel.ob.x = -1;
}

int
tlinelen(Emulator *e, int y)
{
	int i = e->term.col;

	if (e->term.line[y][i - 1].mode & ATTR_WRAP)
		return i;

	while (i > 0 && e->term.line[y][i - 1].u == ' ')
		--i;

	return i;
}

void
selstart(Emulator *e, int col, int row, int snap)
{
	selclear(e);
	e->sel.mode = SEL_EMPTY;
	e->sel.type = SEL_REGULAR;
	e->sel.alt = IS_SET(e, MODE_ALTSCREEN);
	e->sel.snap = snap;
	e->sel.oe.x = e->sel.ob.x = col;
	e->sel.oe.y = e->sel.ob.y = row;
	selnormalize(e);

	if (e->sel.snap != 0)
		e->sel.mode = SEL_READY;
	tsetdirt(e, e->sel.nb.y, e->sel.ne.y);
}

void
selextend(Emulator *e, int col, int row, int type, int done)
{
	int oldey, oldex, oldsby, oldsey, oldtype;

	if (e->sel.mode == SEL_IDLE)
		return;
	if (done && e->sel.mode == SEL_EMPTY)
	{
		selclear(e);
		return;
	}

	oldey = e->sel.oe.y;
	oldex = e->sel.oe.x;
	oldsby = e->sel.nb.y;
	oldsey = e->sel.ne.y;
	oldtype = e->sel.type;

	e->sel.oe.x = col;
	e->sel.oe.y = row;
	selnormalize(e);
	e->sel.type = type;

	if (oldey != e->sel.oe.y || oldex != e->sel.oe.x || oldtype != e->sel.type ||
	    e->sel.mode == SEL_EMPTY)
		tsetdirt(e, MIN(e->sel.nb.y, oldsby), MAX(e->sel.ne.y, oldsey));

	e->sel.mode = done ? SEL_IDLE : SEL_READY;
}

void
selnormalize(Emulator *e)
{
	int i;

	if (e->sel.type == SEL_REGULAR && e->sel.ob.y != e->sel.oe.y)
	{
		e->sel.nb.x = e->sel.ob.y < e->sel.oe.y ? e->sel.ob.x : e->sel.oe.x;
		e->sel.ne.x = e->sel.ob.y < e->sel.oe.y ? e->sel.oe.x : e->sel.ob.x;
	}
	else
	{
		e->sel.nb.x = MIN(e->sel.ob.x, e->sel.oe.x);
		e->sel.ne.x = MAX(e->sel.ob.x, e->sel.oe.x);
	}
	e->sel.nb.y = MIN(e->sel.ob.y, e->sel.oe.y);
	e->sel.ne.y = MAX(e->sel.ob.y, e->sel.oe.y);

	selsnap(e, &e->sel.nb.x, &e->sel.nb.y, -1);
	selsnap(e, &e->sel.ne.x, &e->sel.ne.y, +1);

	// expand selection over line breaks
	if (e->sel.type == SEL_RECTANGULAR)
		return;
	i = tlinelen(e, e->sel.nb.y);
	if (i < e->sel.nb.x)
		e->sel.nb.x = i;
	if (tlinelen(e, e->sel.ne.y) <= e->sel.ne.x)
		e->sel.ne.x = e->term.col - 1;
}

int
selected(Emulator *e, int x, int y)
{
	if (e->sel.mode == SEL_EMPTY || e->sel.ob.x == -1 ||
	    e->sel.alt != IS_SET(e, MODE_ALTSCREEN))
		return 0;

	if (e->sel.type == SEL_RECTANGULAR)
		return between(y, e->sel.nb.y, e->sel.ne.y) && between(x, e->sel.nb.x, e->sel.ne.x);

	return between(y, e->sel.nb.y, e->sel.ne.y) && (y != e->sel.nb.y || x >= e->sel.nb.x) &&
	       (y != e->sel.ne.y || x <= e->sel.ne.x);
}

void
selsnap(Emulator *e, int *x, int *y, int direction)
{
	int newx, newy, xt, yt;
	int delim, prevdelim;
	const Glyph *gp, *prevgp;

	switch (e->sel.snap)
	{
		case SNAP_WORD:
			/*
		   Snap around if the word wraps around at the end or
		   beginning of a line.
		*/
			prevgp = &e->term.line[*y][*x];
			prevdelim = ISDELIM(prevgp->u);
			for (;;)
			{
				newx = *x + direction;
				newy = *y;
				if (!between(newx, 0, e->term.col - 1))
				{
					newy += direction;
					newx = (newx + e->term.col) % e->term.col;
					if (!between(newy, 0, e->term.row - 1))
						break;

					if (direction > 0)
						yt = *y, xt = *x;
					else
						yt = newy, xt = newx;
					if (!(e->term.line[yt][xt].mode & ATTR_WRAP))
						break;
				}

				if (newx >= tlinelen(e, newy))
					break;

				gp = &e->term.line[newy][newx];
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
			*x = (direction < 0) ? 0 : e->term.col - 1;
			if (direction < 0)
			{
				for (; *y > 0; *y += direction)
				{
					if (!(e->term.line[*y - 1][e->term.col - 1].mode &
						ATTR_WRAP))
					{
						break;
					}
				}
			}
			else if (direction > 0)
			{
				for (; *y < e->term.row - 1; *y += direction)
				{
					if (!(e->term.line[*y][e->term.col - 1].mode & ATTR_WRAP))
					{
						break;
					}
				}
			}
			break;
	}
}

char *
getsel(Emulator *e)
{
	char *str, *ptr;
	int y, bufsize, lastx, linelen;
	const Glyph *gp, *last;

	if (e->sel.ob.x == -1)
		return NULL;

	bufsize = (e->term.col + 1) * (e->sel.ne.y - e->sel.nb.y + 1) * UTF_SIZ;
	ptr = str = xmalloc(bufsize);

	// append every set & selected glyph to the selection
	for (y = e->sel.nb.y; y <= e->sel.ne.y; y++)
	{
		if ((linelen = tlinelen(e, y)) == 0)
		{
			*ptr++ = '\n';
			continue;
		}

		if (e->sel.type == SEL_RECTANGULAR)
		{
			gp = &e->term.line[y][e->sel.nb.x];
			lastx = e->sel.ne.x;
		}
		else
		{
			gp = &e->term.line[y][e->sel.nb.y == y ? e->sel.nb.x : 0];
			lastx = (e->sel.ne.y == y) ? e->sel.ne.x : e->term.col - 1;
		}
		last = &e->term.line[y][MIN(lastx, linelen - 1)];
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
		if ((y < e->sel.ne.y || lastx >= linelen) &&
		    (!(last->mode & ATTR_WRAP) || e->sel.type == SEL_RECTANGULAR))
			*ptr++ = '\n';
	}
	*ptr = 0;
	return str;
}

void
selclear(Emulator *e)
{
	if (e->sel.ob.x == -1)
		return;
	e->sel.mode = SEL_IDLE;
	e->sel.ob.x = -1;
	tsetdirt(e, e->sel.nb.y, e->sel.ne.y);
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
   it marks just this emulator dead and hands the message to the host via
   cb_die(), so one terminal failing cannot take down the others. Callers
   must return promptly after invoking it. */
static void
emu_die(Emulator *e, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (e && e->host)
		cb_die(e, buf);
	else
		fputs(buf, stderr);
	if (e)
		e->alive = 0;
}

#ifdef _WIN32
/* ----------------------------------------------------------------------
   Windows ConPTY implementation
   ---------------------------------------------------------------------- */
static unsigned __stdcall
w32_monitor_thread(void *arg)
{
	Emulator *e = (Emulator *) arg;
	WaitForSingleObject(e->w32_proc, INFINITE);
	WaitForSingleObject(e->w32_reader_ready, 5000);
	if (e->w32_hpc)
	{
		ClosePseudoConsole(e->w32_hpc);
		e->w32_hpc = NULL;
	}
	return 0;
}

/* Build child command line from args[], quoting args with spaces.
   Returns 0 on success, -1 if the command line would overflow buf. */
static int
w32_build_cmdline(Emulator *e, char *buf, size_t cap, char *cmd, char **args)
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
				emu_die(e, "ttynew: command line exceeds %zu bytes\n", cap);
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
		emu_die(e, "ttynew: command line exceeds %zu bytes\n", cap);
		return -1;
	}
	return 0;
}

/* Spawn child attached to ConPTY via STARTUPINFOEXW.
   Returns 0 on success, -1 on failure (no process created). */
static int
w32_spawn_pcon_child(Emulator *e, LPWSTR cmdline, HPCON hpc, HANDLE *out_proc)
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
		emu_die(e, "malloc(%zu) for attribute list failed\n", (size_t) attr_sz);
		return -1;
	}
	if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_sz))
	{
		emu_die(e, "InitializeProcThreadAttributeList failed: %lu\n", GetLastError());
		free(si.lpAttributeList);
		return -1;
	}
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
		hpc, sizeof(HPCON), NULL, NULL))
	{
		emu_die(e, "UpdateProcThreadAttribute failed: %lu\n", GetLastError());
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
		return -1;
	}

	if (!CreateProcessW(NULL, cmdline, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT, NULL,
		NULL, &si.StartupInfo, &pi))
	{
		emu_die(e, "CreateProcessW failed: %lu\n", GetLastError());
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
static void
w32_ttynew_cleanup(Emulator *e)
{
	if (e->w32_proc)
	{
		TerminateProcess(e->w32_proc, 1);
		CloseHandle(e->w32_proc);
		e->w32_proc = NULL;
	}
	if (e->w32_hpc)
	{
		ClosePseudoConsole(e->w32_hpc);
		e->w32_hpc = NULL;
	}
	if (e->w32_pipe_in)
	{
		CloseHandle(e->w32_pipe_in);
		e->w32_pipe_in = NULL;
	}
	if (e->w32_pipe_out)
	{
		CloseHandle(e->w32_pipe_out);
		e->w32_pipe_out = NULL;
	}
	if (e->w32_reader_ready)
	{
		CloseHandle(e->w32_reader_ready);
		e->w32_reader_ready = NULL;
	}
}

int
ttynew(Emulator *e, const char *line, char *cmd, const char *out, char **args)
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
		e->term.mode |= MODE_PRINT;
		e->iofd = (!strcmp(out, "-")) ? 1 : _open(out, O_WRONLY | O_CREAT, 0666);
		if (e->iofd < 0)
			fprintf(stderr, "Error opening %s:%s\n", out, strerror(errno));
	}

	/* Two pipe pairs for ConPTY <-> child. SECURITY_ATTRIBUTES with
	   bInheritHandle=TRUE is required, ConPTY duplicates the inner
	   ends into conhost.exe internally. */
	if (!CreatePipe(&pipe_in_r, &pipe_in_w, &sa, 0))
	{
		emu_die(e, "CreatePipe (in) failed: %lu\n", GetLastError());
		return -1;
	}
	if (!CreatePipe(&pipe_out_r, &pipe_out_w, &sa, 0))
	{
		emu_die(e, "CreatePipe (out) failed: %lu\n", GetLastError());
		CloseHandle(pipe_in_r);
		CloseHandle(pipe_in_w);
		return -1;
	}

	// Pseudo-console attached to the inner pipe ends.
	size.X = (SHORT) (e->term.col ? e->term.col : 80);
	size.Y = (SHORT) (e->term.row ? e->term.row : 24);
	hr = CreatePseudoConsole(size, pipe_in_r, pipe_out_w, 0, &e->w32_hpc);
	if (FAILED(hr))
	{
		emu_die(e, "CreatePseudoConsole failed: 0x%lx\n", (unsigned long) hr);
		CloseHandle(pipe_in_r);
		CloseHandle(pipe_in_w);
		CloseHandle(pipe_out_r);
		CloseHandle(pipe_out_w);
		return -1;
	}

	// ConPTY owns the inner ends; we keep the outer ends.
	CloseHandle(pipe_in_r);
	CloseHandle(pipe_out_w);
	e->w32_pipe_in = pipe_out_r;
	e->w32_pipe_out = pipe_in_w;

	/* TERM for ncurses-aware programs. MSYS=enable_pcon tells the
	   MSYS2/Cygwin runtime to cooperate with ConPTY rather than
	   bypassing it through its own PTY layer. */
	SetEnvironmentVariableA("TERM", termname);
	SetEnvironmentVariableA("MSYS", "enable_pcon");

	// Build the command line, convert UTF-8 -> UTF-16, spawn.
	if (w32_build_cmdline(e, cmdline, sizeof(cmdline), cmd, args) < 0)
	{
		w32_ttynew_cleanup(e); /* w32_build_cmdline already reported */
		return -1;
	}
	wlen = MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, NULL, 0);
	if (wlen <= 0)
	{
		emu_die(e, "MultiByteToWideChar failed: %lu\n", GetLastError());
		w32_ttynew_cleanup(e);
		return -1;
	}
	wcmd = (wchar_t *) malloc((size_t) wlen * sizeof(wchar_t));
	if (!wcmd)
	{
		emu_die(e, "malloc for wide cmdline failed\n");
		w32_ttynew_cleanup(e);
		return -1;
	}
	MultiByteToWideChar(CP_UTF8, 0, cmdline, -1, wcmd, wlen);

	if (w32_spawn_pcon_child(e, wcmd, e->w32_hpc, &e->w32_proc) < 0)
	{
		free(wcmd);
		w32_ttynew_cleanup(e); /* w32_spawn_pcon_child already reported */
		return -1;
	}
	free(wcmd);

	/* Reader-ready event must exist before the monitor thread starts
	   (the thread waits on it). See w32_monitor_thread comment. */
	e->w32_reader_ready = CreateEvent(NULL, TRUE, FALSE, NULL);
	if (!e->w32_reader_ready)
	{
		emu_die(e, "CreateEvent failed: %lu\n", GetLastError());
		w32_ttynew_cleanup(e);
		return -1;
	}

	if (_beginthreadex(NULL, 0, w32_monitor_thread, e, 0, NULL) == 0)
	{
		emu_die(e, "_beginthreadex(monitor) failed: %s\n", strerror(errno));
		w32_ttynew_cleanup(e);
		return -1;
	}

	// e->cmdfd is opaque on Windows, just needs to be >= 0.
	e->cmdfd = 1;
	e->alive = 1;
	return e->cmdfd;
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
		args = (char *[]){argv0, arg, NULL};

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
	   instance reaps its own e->pid (see ttyread EOF and ttyhangup). This
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
ttynew(Emulator *e, const char *line, char *cmd, const char *out, char **args)
{
	int m, s;

	if (out)
	{
		e->term.mode |= MODE_PRINT;
		e->iofd = (!strcmp(out, "-")) ? 1 : open(out, O_WRONLY | O_CREAT, 0666);
		if (e->iofd < 0)
		{
			fprintf(stderr, "Error opening %s:%s\n", out, strerror(errno));
		}
	}

	if (line)
	{
		if ((e->cmdfd = open(line, O_RDWR)) < 0)
		{
			emu_die(e, "open line '%s' failed: %s\n", line, strerror(errno));
			return -1;
		}
		dup2(e->cmdfd, 0);
		stty(args);
		return e->cmdfd;
	}

	// seems to work fine on linux, openbsd and freebsd
	if (openpty(&m, &s, NULL, NULL, NULL) < 0)
	{
		emu_die(e, "openpty failed: %s\n", strerror(errno));
		return -1;
	}

	switch (e->pid = fork())
	{
		case -1:
			emu_die(e, "fork failed: %s\n", strerror(errno));
			return -1;
		case 0:
			close(e->iofd);
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
				emu_die(e, "pledge\n");
				return -1;
			}
#endif
			close(s);
			e->cmdfd = m;
			e->alive = 1;
			signal(SIGCHLD, sigchld);
			break;
	}
	return e->cmdfd;
}
#endif /* !_WIN32 */

#ifdef _WIN32
size_t
ttyread(Emulator *e)
{
	int written;
	if (!e->w32_reader_signaled)
	{
		SetEvent(e->w32_reader_ready);
		e->w32_reader_signaled = 1;
	}

	DWORD dwRead = 0;
	if (!ReadFile(e->w32_pipe_in, e->read_buf + e->read_buflen,
		LEN(e->read_buf) - e->read_buflen, &dwRead, NULL) ||
	    dwRead == 0)
	{
		// Pipe broken by ClosePseudoConsole. Drain residual.
		for (;;)
		{
			if (!ReadFile(e->w32_pipe_in, e->read_buf + e->read_buflen,
				LEN(e->read_buf) - e->read_buflen, &dwRead, NULL) ||
			    dwRead == 0)
				break;
			e->read_buflen += (int) dwRead;
			written = twrite(e, e->read_buf, e->read_buflen, 0);
			e->read_buflen -= written;
			if (e->read_buflen > 0)
				memmove(e->read_buf, e->read_buf + written, e->read_buflen);
		}
		e->alive = 0;
		return 0;
	}
	e->read_buflen += (int) dwRead;
	written = twrite(e, e->read_buf, e->read_buflen, 0);
	e->read_buflen -= written;
	if (e->read_buflen > 0)
		memmove(e->read_buf, e->read_buf + written, e->read_buflen);
	return (size_t) dwRead;
}
#else
size_t
ttyread(Emulator *e)
{
	int ret, written;

	ret = read(e->cmdfd, e->read_buf + e->read_buflen, LEN(e->read_buf) - e->read_buflen);

	switch (ret)
	{
		case 0:
			/* EOF on the pty master: the child has closed its end, i.e.
			   it has exited. Reap this instance's own child so it does
			   not linger as a zombie. */
			e->alive = 0;
			if (e->pid > 0)
			{
				waitpid(e->pid, NULL, WNOHANG);
				e->pid = 0;
			}
			return 0;
		case -1:
			emu_die(e, "couldn't read from shell: %s\n", strerror(errno));
			return 0;
		default:
			e->read_buflen += ret;
			written = twrite(e, e->read_buf, e->read_buflen, 0);
			e->read_buflen -= written;
			// keep any incomplete UTF-8 byte sequence for the next call
			if (e->read_buflen > 0)
				memmove(e->read_buf, e->read_buf + written, e->read_buflen);
			return ret;
	}
}
#endif

void
ttywrite(Emulator *e, const char *s, size_t n, int may_echo)
{
	const char *next;

	if (may_echo && IS_SET(e, MODE_ECHO))
		twrite(e, s, n, 1);

	if (!IS_SET(e, MODE_CRLF))
	{
		ttywriteraw(e, s, n);
		return;
	}

	// This is similar to how the kernel handles ONLCR for ttys
	while (n > 0)
	{
		if (*s == '\r')
		{
			next = s + 1;
			ttywriteraw(e, "\r\n", 2);
		}
		else
		{
			next = memchr(s, '\r', n);
			DEFAULT(next, s + n);
			ttywriteraw(e, s, next - s);
		}
		n -= next - s;
		s = next;
	}
}

void
ttywriteraw(Emulator *e, const char *s, size_t n)
{
#ifdef _WIN32
	/* Windows ConPTY: WriteFile on the pipe. Simpler than the Unix
	   pselect() dance because ConPTY handles flow control internally. */
	while (n > 0)
	{
		DWORD written = 0;
		if (!WriteFile(e->w32_pipe_out, s, (DWORD) n, &written, NULL))
		{
			emu_die(e, "write error on tty: %lu\n", GetLastError());
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
		FD_SET(e->cmdfd, &wfd);
		FD_SET(e->cmdfd, &rfd);

		// Check if we can write.
		if (pselect(e->cmdfd + 1, &rfd, &wfd, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR)
				continue;
			emu_die(e, "select failed: %s\n", strerror(errno));
			return;
		}
		if (FD_ISSET(e->cmdfd, &wfd))
		{
			/*
			   Only write the bytes written by ttywrite(e) or the
			   default of 256. This seems to be a reasonable value
			   for a serial line. Bigger values might clog the I/O.
			*/
			if ((r = write(e->cmdfd, s, (n < lim) ? n : lim)) < 0)
				goto write_error;
			if (r < n)
			{
				/*
				   We weren't able to write out everything.
				   This means the buffer is getting full
				   again. Empty it.
				*/
				if (n < lim)
					lim = ttyread(e);
				n -= r;
				s += r;
			}
			else
			{
				// All bytes have been written.
				break;
			}
		}
		if (FD_ISSET(e->cmdfd, &rfd))
			lim = ttyread(e);
	}
	return;

write_error:
	emu_die(e, "write error on tty: %s\n", strerror(errno));
#endif
}

void
ttyresize(Emulator *e, int tw, int th)
{
#ifdef _WIN32
	COORD size;
	size.X = (SHORT) e->term.col;
	size.Y = (SHORT) e->term.row;
	if (e->w32_hpc)
		ResizePseudoConsole(e->w32_hpc, size);
#else
	struct winsize w;

	w.ws_row = e->term.row;
	w.ws_col = e->term.col;
	w.ws_xpixel = tw;
	w.ws_ypixel = th;
	if (ioctl(e->cmdfd, TIOCSWINSZ, &w) < 0)
		fprintf(stderr, "Couldn't set window size: %s\n", strerror(errno));
#endif
}

void
ttyhangup(Emulator *e)
{
#ifdef _WIN32
	if (e->w32_proc)
	{
		TerminateProcess(e->w32_proc, 1);
		CloseHandle(e->w32_proc);
		e->w32_proc = NULL;
	}
	if (e->w32_hpc)
	{
		ClosePseudoConsole(e->w32_hpc);
		e->w32_hpc = NULL;
	}
#else
	// Send SIGHUP to shell, then reap this instance's own child so it
	// does not become a zombie (the shared sigchld handler no longer reaps).
	if (e->pid > 0)
	{
		kill(e->pid, SIGHUP);
		waitpid(e->pid, NULL, 0);
		e->pid = 0;
		e->alive = 0;
	}
#endif
}

int
tattrset(Emulator *e, int attr)
{
	int i, j;

	for (i = 0; i < e->term.row - 1; i++)
	{
		for (j = 0; j < e->term.col - 1; j++)
		{
			if (e->term.line[i][j].mode & attr)
				return 1;
		}
	}

	return 0;
}

void
tsetdirt(Emulator *e, int top, int bot)
{
	int i;

	if (e->term.row <= 0)
		return;

	LIMIT(top, 0, e->term.row - 1);
	LIMIT(bot, 0, e->term.row - 1);

	for (i = top; i <= bot; i++)
		e->term.dirty[i] = 1;
}

void
tsetdirtattr(Emulator *e, int attr)
{
	int i, j;

	for (i = 0; i < e->term.row - 1; i++)
	{
		for (j = 0; j < e->term.col - 1; j++)
		{
			if (e->term.line[i][j].mode & attr)
			{
				tsetdirt(e, i, i);
				break;
			}
		}
	}
}

void
tfulldirt(Emulator *e)
{
	tsetdirt(e, 0, e->term.row - 1);
}

void
tcursor(Emulator *e, int mode)
{
	int alt = IS_SET(e, MODE_ALTSCREEN);

	if (mode == CURSOR_SAVE)
	{
		e->saved_cursor[alt] = e->term.c;
	}
	else if (mode == CURSOR_LOAD)
	{
		e->term.c = e->saved_cursor[alt];
		tmoveto(e, e->saved_cursor[alt].x, e->saved_cursor[alt].y);
	}
}

void
treset(Emulator *e)
{
	uint i;

	e->term.c = (TCursor){{.mode = ATTR_NULL, .fg = defaultfg, .bg = defaultbg}, .x = 0, .y = 0,
	    .state = CURSOR_DEFAULT};

	memset(e->term.tabs, 0, e->term.col * sizeof(*e->term.tabs));
	for (i = tabspaces; i < e->term.col; i += tabspaces)
		e->term.tabs[i] = 1;
	e->term.top = 0;
	e->term.bot = e->term.row - 1;
	e->term.mode = MODE_WRAP | MODE_UTF8;
	memset(e->term.trantbl, CS_USA, sizeof(e->term.trantbl));
	e->term.charset = 0;

	for (i = 0; i < 2; i++)
	{
		tmoveto(e, 0, 0);
		tcursor(e, CURSOR_SAVE);
		tclearregion(e, 0, 0, e->term.col - 1, e->term.row - 1);
		tswapscreen(e);
	}
}

void
tnew(Emulator *e, int col, int row)
{
	e->term = (Term){.c = {.attr = {.fg = defaultfg, .bg = defaultbg}}};
	tresize(e, col, row);
	treset(e);
}

void
tswapscreen(Emulator *e)
{
	Line *tmp = e->term.line;

	e->term.line = e->term.alt;
	e->term.alt = tmp;
	e->term.mode ^= MODE_ALTSCREEN;
	tfulldirt(e);
}

void
tscrolldown(Emulator *e, int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, e->term.bot - orig + 1);

	tsetdirt(e, orig, e->term.bot - n);
	tclearregion(e, 0, e->term.bot - n + 1, e->term.col - 1, e->term.bot);

	for (i = e->term.bot; i >= orig + n; i--)
	{
		temp = e->term.line[i];
		e->term.line[i] = e->term.line[i - n];
		e->term.line[i - n] = temp;
	}

	selscroll(e, orig, n);
}

void
tscrollup(Emulator *e, int orig, int n)
{
	int i;
	Line temp;

	LIMIT(n, 0, e->term.bot - orig + 1);

	tclearregion(e, 0, orig, e->term.col - 1, orig + n - 1);
	tsetdirt(e, orig + n, e->term.bot);

	for (i = orig; i <= e->term.bot - n; i++)
	{
		temp = e->term.line[i];
		e->term.line[i] = e->term.line[i + n];
		e->term.line[i + n] = temp;
	}

	selscroll(e, orig, -n);
}

void
selscroll(Emulator *e, int orig, int n)
{
	if (e->sel.ob.x == -1 || e->sel.alt != IS_SET(e, MODE_ALTSCREEN))
		return;

	if (between(e->sel.nb.y, orig, e->term.bot) != between(e->sel.ne.y, orig, e->term.bot))
	{
		selclear(e);
	}
	else if (between(e->sel.nb.y, orig, e->term.bot))
	{
		e->sel.ob.y += n;
		e->sel.oe.y += n;
		if (e->sel.ob.y < e->term.top || e->sel.ob.y > e->term.bot ||
		    e->sel.oe.y < e->term.top || e->sel.oe.y > e->term.bot)
		{
			selclear(e);
		}
		else
		{
			selnormalize(e);
		}
	}
}

void
tnewline(Emulator *e, int first_col)
{
	int y = e->term.c.y;

	if (y == e->term.bot)
	{
		tscrollup(e, e->term.top, 1);
	}
	else
	{
		y++;
	}
	tmoveto(e, first_col ? 0 : e->term.c.x, y);
}

void
csiparse(Emulator *e)
{
	char *p = e->csi.buf, *np;
	long int v;
	int sep = ';'; /* colon or semi-colon, but not both */

	e->csi.narg = 0;
	if (*p == '?')
	{
		e->csi.priv = 1;
		p++;
	}

	e->csi.buf[e->csi.len] = '\0';
	while (p < e->csi.buf + e->csi.len)
	{
		np = NULL;
		v = strtol(p, &np, 10);
		if (np == p)
			v = 0;
		if (v == LONG_MAX || v == LONG_MIN)
			v = -1;
		e->csi.arg[e->csi.narg++] = v;
		p = np;
		if (sep == ';' && *p == ':')
			sep = ':'; /* allow override to colon once */
		if (*p != sep || e->csi.narg == ESC_ARG_SIZ)
			break;
		p++;
	}
	e->csi.mode[0] = *p++;
	e->csi.mode[1] = (p < e->csi.buf + e->csi.len) ? *p : '\0';
}

// for absolute user moves, when decom is set
void
tmoveato(Emulator *e, int x, int y)
{
	tmoveto(e, x, y + ((e->term.c.state & CURSOR_ORIGIN) ? e->term.top : 0));
}

void
tmoveto(Emulator *e, int x, int y)
{
	int miny, maxy;

	if (e->term.c.state & CURSOR_ORIGIN)
	{
		miny = e->term.top;
		maxy = e->term.bot;
	}
	else
	{
		miny = 0;
		maxy = e->term.row - 1;
	}
	e->term.c.state &= ~CURSOR_WRAPNEXT;
	e->term.c.x = LIMIT(x, 0, e->term.col - 1);
	e->term.c.y = LIMIT(y, miny, maxy);
}

void
tsetchar(Emulator *e, Rune u, const Glyph *attr, int x, int y)
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
	if (e->term.trantbl[e->term.charset] == CS_GRAPHIC0 && between(u, 0x41, 0x7e) &&
	    vt100_0[u - 0x41])
		utf8decode(vt100_0[u - 0x41], &u, UTF_SIZ);

	if (e->term.line[y][x].mode & ATTR_WIDE)
	{
		if (x + 1 < e->term.col)
		{
			e->term.line[y][x + 1].u = ' ';
			e->term.line[y][x + 1].mode &= ~ATTR_WDUMMY;
		}
	}
	else if (e->term.line[y][x].mode & ATTR_WDUMMY)
	{
		e->term.line[y][x - 1].u = ' ';
		e->term.line[y][x - 1].mode &= ~ATTR_WIDE;
	}

	e->term.dirty[y] = 1;
	e->term.line[y][x] = *attr;
	e->term.line[y][x].u = u;
}

void
tclearregion(Emulator *e, int x1, int y1, int x2, int y2)
{
	int x, y, temp;
	Glyph *gp;

	if (x1 > x2)
		temp = x1, x1 = x2, x2 = temp;
	if (y1 > y2)
		temp = y1, y1 = y2, y2 = temp;

	LIMIT(x1, 0, e->term.col - 1);
	LIMIT(x2, 0, e->term.col - 1);
	LIMIT(y1, 0, e->term.row - 1);
	LIMIT(y2, 0, e->term.row - 1);

	for (y = y1; y <= y2; y++)
	{
		e->term.dirty[y] = 1;
		for (x = x1; x <= x2; x++)
		{
			gp = &e->term.line[y][x];
			if (selected(e, x, y))
				selclear(e);
			gp->fg = e->term.c.attr.fg;
			gp->bg = e->term.c.attr.bg;
			gp->mode = 0;
			gp->u = ' ';
		}
	}
}

void
tdeletechar(Emulator *e, int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, e->term.col - e->term.c.x);

	dst = e->term.c.x;
	src = e->term.c.x + n;
	size = e->term.col - src;
	line = e->term.line[e->term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(e, e->term.col - n, e->term.c.y, e->term.col - 1, e->term.c.y);
}

void
tinsertblank(Emulator *e, int n)
{
	int dst, src, size;
	Glyph *line;

	LIMIT(n, 0, e->term.col - e->term.c.x);

	dst = e->term.c.x + n;
	src = e->term.c.x;
	size = e->term.col - dst;
	line = e->term.line[e->term.c.y];

	memmove(&line[dst], &line[src], size * sizeof(Glyph));
	tclearregion(e, src, e->term.c.y, dst - 1, e->term.c.y);
}

void
tinsertblankline(Emulator *e, int n)
{
	if (between(e->term.c.y, e->term.top, e->term.bot))
		tscrolldown(e, e->term.c.y, n);
}

void
tdeleteline(Emulator *e, int n)
{
	if (between(e->term.c.y, e->term.top, e->term.bot))
		tscrollup(e, e->term.c.y, n);
}

int32_t
tdefcolor(Emulator *e, const int *attr, int *npar, int l)
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
tsetattr(Emulator *e, const int *attr, int l)
{
	int i;
	int32_t idx;

	for (i = 0; i < l; i++)
	{
		switch (attr[i])
		{
			case 0:
				e->term.c.attr.mode &=
				    ~(ATTR_BOLD | ATTR_FAINT | ATTR_ITALIC | ATTR_UNDERLINE |
					ATTR_BLINK | ATTR_REVERSE | ATTR_INVISIBLE | ATTR_STRUCK);
				e->term.c.attr.fg = defaultfg;
				e->term.c.attr.bg = defaultbg;
				break;
			case 1:
				e->term.c.attr.mode |= ATTR_BOLD;
				break;
			case 2:
				e->term.c.attr.mode |= ATTR_FAINT;
				break;
			case 3:
				e->term.c.attr.mode |= ATTR_ITALIC;
				break;
			case 4:
				e->term.c.attr.mode |= ATTR_UNDERLINE;
				break;
			case 5: /* slow blink */
				/* FALLTHROUGH */
			case 6: /* rapid blink */
				e->term.c.attr.mode |= ATTR_BLINK;
				break;
			case 7:
				e->term.c.attr.mode |= ATTR_REVERSE;
				break;
			case 8:
				e->term.c.attr.mode |= ATTR_INVISIBLE;
				break;
			case 9:
				e->term.c.attr.mode |= ATTR_STRUCK;
				break;
			case 22:
				e->term.c.attr.mode &= ~(ATTR_BOLD | ATTR_FAINT);
				break;
			case 23:
				e->term.c.attr.mode &= ~ATTR_ITALIC;
				break;
			case 24:
				e->term.c.attr.mode &= ~ATTR_UNDERLINE;
				break;
			case 25:
				e->term.c.attr.mode &= ~ATTR_BLINK;
				break;
			case 27:
				e->term.c.attr.mode &= ~ATTR_REVERSE;
				break;
			case 28:
				e->term.c.attr.mode &= ~ATTR_INVISIBLE;
				break;
			case 29:
				e->term.c.attr.mode &= ~ATTR_STRUCK;
				break;
			case 38:
				if ((idx = tdefcolor(e, attr, &i, l)) >= 0)
					e->term.c.attr.fg = idx;
				break;
			case 39: /* set foreground color to default */
				e->term.c.attr.fg = defaultfg;
				break;
			case 48:
				if ((idx = tdefcolor(e, attr, &i, l)) >= 0)
					e->term.c.attr.bg = idx;
				break;
			case 49: /* set background color to default */
				e->term.c.attr.bg = defaultbg;
				break;
			case 58:
				/*
			   This starts a sequence to change the color of
			   "underline" pixels. We don't support that and
			   instead eat up a following "5;n" or "2;r;g;b".
			*/
				tdefcolor(e, attr, &i, l);
				break;
			default:
				if (between(attr[i], 30, 37))
				{
					e->term.c.attr.fg = attr[i] - 30;
				}
				else if (between(attr[i], 40, 47))
				{
					e->term.c.attr.bg = attr[i] - 40;
				}
				else if (between(attr[i], 90, 97))
				{
					e->term.c.attr.fg = attr[i] - 90 + 8;
				}
				else if (between(attr[i], 100, 107))
				{
					e->term.c.attr.bg = attr[i] - 100 + 8;
				}
				else
				{
					fprintf(stderr, "erresc(default): gfx attr %d unknown\n",
					    attr[i]);
					csidump(e);
				}
				break;
		}
	}
}

void
tsetscroll(Emulator *e, int t, int b)
{
	int temp;

	LIMIT(t, 0, e->term.row - 1);
	LIMIT(b, 0, e->term.row - 1);
	if (t > b)
	{
		temp = t;
		t = b;
		b = temp;
	}
	e->term.top = t;
	e->term.bot = b;
}

void
tsetmode(Emulator *e, int priv, int set, const int *args, int narg)
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
					cb_setmode(e, set, MODE_APPCURSOR);
					break;
				case 5: /* DECSCNM -- Reverse video */
					cb_setmode(e, set, MODE_REVERSE);
					break;
				case 6: /* DECOM -- Origin */
					e->term.c.state =
					    (char) modbit(e->term.c.state, set, CURSOR_ORIGIN);
					tmoveato(e, 0, 0);
					break;
				case 7: /* DECAWM -- Auto wrap */
					e->term.mode = modbit(e->term.mode, set, MODE_WRAP);
					break;
				case 0:	 /* Error (IGNORED) */
				case 2:	 /* DECANM -- ANSI/VT52 (IGNORED) */
				case 3:	 /* DECCOLM -- Column  (IGNORED) */
				case 4:	 /* DECSCLM -- Scroll (IGNORED) */
				case 8:	 /* DECARM -- Auto repeat (IGNORED) */
				case 18: /* DECPFF -- Printer feed (IGNORED) */
				case 19: /* DECPEX -- Printer extent (IGNORED) */
				case 42: /* DECNRCM -- National characters (IGNORED) */
				case 12: /* att610 -- Start blinking cursor (IGNORED) */
					break;
				case 25: /* DECTCEM -- Text Cursor Enable Mode */
					cb_setmode(e, !set, MODE_HIDE);
					break;
				case 9: /* X10 mouse compatibility mode */
					cb_setpointermotion(e, 0);
					cb_setmode(e, 0, MODE_MOUSE);
					cb_setmode(e, set, MODE_MOUSEX10);
					break;
				case 1000: /* 1000: report button press */
					cb_setpointermotion(e, 0);
					cb_setmode(e, 0, MODE_MOUSE);
					cb_setmode(e, set, MODE_MOUSEBTN);
					break;
				case 1002: /* 1002: report motion on button press */
					cb_setpointermotion(e, 0);
					cb_setmode(e, 0, MODE_MOUSE);
					cb_setmode(e, set, MODE_MOUSEMOTION);
					break;
				case 1003: /* 1003: enable all mouse motions */
					cb_setpointermotion(e, set);
					cb_setmode(e, 0, MODE_MOUSE);
					cb_setmode(e, set, MODE_MOUSEMANY);
					break;
				case 1004: /* 1004: send focus events to tty */
					cb_setmode(e, set, MODE_FOCUS);
					break;
				case 1006: /* 1006: extended reporting mode */
					cb_setmode(e, set, MODE_MOUSESGR);
					break;
				case 1034: /* 1034: enable 8-bit mode for keyboard input */
					cb_setmode(e, set, MODE_8BIT);
					break;
				case 1049: /* swap screen & set/restore cursor as xterm */
					if (!allowaltscreen)
						break;
					tcursor(e, (set) ? CURSOR_SAVE : CURSOR_LOAD);
					/* FALLTHROUGH */
				case 47:   /* swap screen buffer */
				case 1047: /* swap screen buffer */
					if (!allowaltscreen)
						break;
					alt = IS_SET(e, MODE_ALTSCREEN);
					if (alt)
					{
						tclearregion(
						    e, 0, 0, e->term.col - 1, e->term.row - 1);
					}
					if (set ^ alt) /* set is always 1 or 0 */
						tswapscreen(e);
					if (*args != 1049)
						break;
					/* FALLTHROUGH */
				case 1048: /* save/restore cursor (like DECSC/DECRC) */
					tcursor(e, (set) ? CURSOR_SAVE : CURSOR_LOAD);
					break;
				case 2004: /* 2004: bracketed paste mode */
					cb_setmode(e, set, MODE_BRCKTPASTE);
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
					cb_setmode(e, set, MODE_KBDLOCK);
					break;
				case 4: /* IRM -- Insertion-replacement */
					e->term.mode = modbit(e->term.mode, set, MODE_INSERT);
					break;
				case 12: /* SRM -- Send/Receive */
					e->term.mode = modbit(e->term.mode, !set, MODE_ECHO);
					break;
				case 20: /* LNM -- Linefeed/new line */
					e->term.mode = modbit(e->term.mode, set, MODE_CRLF);
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
csihandle(Emulator *e)
{
	char buf[40];
	int len;

	switch (e->csi.mode[0])
	{
		default:
		unknown:
			fprintf(stderr, "erresc: unknown csi ");
			csidump(e);
			/* die(""); */
			break;
		case '@': /* ICH -- Insert <n> blank char */
			DEFAULT(e->csi.arg[0], 1);
			tinsertblank(e, e->csi.arg[0]);
			break;
		case 'A': /* CUU -- Cursor <n> Up */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, e->term.c.x, e->term.c.y - e->csi.arg[0]);
			break;
		case 'B': /* CUD -- Cursor <n> Down */
		case 'e': /* VPR --Cursor <n> Down */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, e->term.c.x, e->term.c.y + e->csi.arg[0]);
			break;
		case 'i': /* MC -- Media Copy */
			switch (e->csi.arg[0])
			{
				case 0:
					tdump(e);
					break;
				case 1:
					tdumpline(e, e->term.c.y);
					break;
				case 2:
					tdumpsel(e);
					break;
				case 4:
					e->term.mode &= ~MODE_PRINT;
					break;
				case 5:
					e->term.mode |= MODE_PRINT;
					break;
			}
			break;
		case 'c': /* DA -- Device Attributes */
			if (e->csi.arg[0] == 0)
				ttywrite(e, vtiden, strlen(vtiden), 0);
			break;
		case 'b': /* REP -- if last char is printable print it <n> more times */
			LIMIT(e->csi.arg[0], 1, 65535);
			if (e->term.lastc)
				while (e->csi.arg[0]-- > 0)
					tputc(e, e->term.lastc);
			break;
		case 'C': /* CUF -- Cursor <n> Forward */
		case 'a': /* HPR -- Cursor <n> Forward */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, e->term.c.x + e->csi.arg[0], e->term.c.y);
			break;
		case 'D': /* CUB -- Cursor <n> Backward */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, e->term.c.x - e->csi.arg[0], e->term.c.y);
			break;
		case 'E': /* CNL -- Cursor <n> Down and first col */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, 0, e->term.c.y + e->csi.arg[0]);
			break;
		case 'F': /* CPL -- Cursor <n> Up and first col */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, 0, e->term.c.y - e->csi.arg[0]);
			break;
		case 'g': /* TBC -- Tabulation clear */
			switch (e->csi.arg[0])
			{
				case 0: /* clear current tab stop */
					e->term.tabs[e->term.c.x] = 0;
					break;
				case 3: /* clear all the tabs */
					memset(
					    e->term.tabs, 0, e->term.col * sizeof(*e->term.tabs));
					break;
				default:
					goto unknown;
			}
			break;
		case 'G': /* CHA -- Move to <col> */
		case '`': /* HPA */
			DEFAULT(e->csi.arg[0], 1);
			tmoveto(e, e->csi.arg[0] - 1, e->term.c.y);
			break;
		case 'H': /* CUP -- Move to <row> <col> */
		case 'f': /* HVP */
			DEFAULT(e->csi.arg[0], 1);
			DEFAULT(e->csi.arg[1], 1);
			tmoveato(e, e->csi.arg[1] - 1, e->csi.arg[0] - 1);
			break;
		case 'I': /* CHT -- Cursor Forward Tabulation <n> tab stops */
			DEFAULT(e->csi.arg[0], 1);
			tputtab(e, e->csi.arg[0]);
			break;
		case 'J': /* ED -- Clear screen */
			switch (e->csi.arg[0])
			{
				case 0: /* below */
					tclearregion(e, e->term.c.x, e->term.c.y, e->term.col - 1,
					    e->term.c.y);
					if (e->term.c.y < e->term.row - 1)
					{
						tclearregion(e, 0, e->term.c.y + 1, e->term.col - 1,
						    e->term.row - 1);
					}
					break;
				case 1: /* above */
					if (e->term.c.y > 0)
						tclearregion(
						    e, 0, 0, e->term.col - 1, e->term.c.y - 1);
					tclearregion(e, 0, e->term.c.y, e->term.c.x, e->term.c.y);
					break;
				case 2: /* all */
					tclearregion(e, 0, 0, e->term.col - 1, e->term.row - 1);
					break;
				default:
					goto unknown;
			}
			break;
		case 'K': /* EL -- Clear line */
			switch (e->csi.arg[0])
			{
				case 0: /* right */
					tclearregion(e, e->term.c.x, e->term.c.y, e->term.col - 1,
					    e->term.c.y);
					break;
				case 1: /* left */
					tclearregion(e, 0, e->term.c.y, e->term.c.x, e->term.c.y);
					break;
				case 2: /* all */
					tclearregion(
					    e, 0, e->term.c.y, e->term.col - 1, e->term.c.y);
					break;
			}
			break;
		case 'S': /* SU -- Scroll <n> line up */
			if (e->csi.priv)
				break;
			DEFAULT(e->csi.arg[0], 1);
			tscrollup(e, e->term.top, e->csi.arg[0]);
			break;
		case 'T': /* SD -- Scroll <n> line down */
			DEFAULT(e->csi.arg[0], 1);
			tscrolldown(e, e->term.top, e->csi.arg[0]);
			break;
		case 'L': /* IL -- Insert <n> blank lines */
			DEFAULT(e->csi.arg[0], 1);
			tinsertblankline(e, e->csi.arg[0]);
			break;
		case 'l': /* RM -- Reset Mode */
			tsetmode(e, e->csi.priv, 0, e->csi.arg, e->csi.narg);
			break;
		case 'M': /* DL -- Delete <n> lines */
			DEFAULT(e->csi.arg[0], 1);
			tdeleteline(e, e->csi.arg[0]);
			break;
		case 'X': /* ECH -- Erase <n> char */
			DEFAULT(e->csi.arg[0], 1);
			tclearregion(e, e->term.c.x, e->term.c.y, e->term.c.x + e->csi.arg[0] - 1,
			    e->term.c.y);
			break;
		case 'P': /* DCH -- Delete <n> char */
			DEFAULT(e->csi.arg[0], 1);
			tdeletechar(e, e->csi.arg[0]);
			break;
		case 'Z': /* CBT -- Cursor Backward Tabulation <n> tab stops */
			DEFAULT(e->csi.arg[0], 1);
			tputtab(e, -e->csi.arg[0]);
			break;
		case 'd': /* VPA -- Move to <row> */
			DEFAULT(e->csi.arg[0], 1);
			tmoveato(e, e->term.c.x, e->csi.arg[0] - 1);
			break;
		case 'h': /* SM -- Set terminal mode */
			tsetmode(e, e->csi.priv, 1, e->csi.arg, e->csi.narg);
			break;
		case 'm': /* SGR -- Terminal attribute (color) */
			tsetattr(e, e->csi.arg, e->csi.narg);
			break;
		case 'n': /* DSR -- Device Status Report */
			switch (e->csi.arg[0])
			{
				case 5: /* Status Report "OK" `0n` */
					ttywrite(e, "\033[0n", sizeof("\033[0n") - 1, 0);
					break;
				case 6: /* Report Cursor Position (CPR) "<row>;<column>R" */
					len = snprintf(buf, sizeof(buf), "\033[%i;%iR",
					    e->term.c.y + 1, e->term.c.x + 1);
					ttywrite(e, buf, len, 0);
					break;
				default:
					goto unknown;
			}
			break;
		case 'r': /* DECSTBM -- Set Scrolling Region */
			if (e->csi.priv)
			{
				goto unknown;
			}
			else
			{
				DEFAULT(e->csi.arg[0], 1);
				DEFAULT(e->csi.arg[1], e->term.row);
				tsetscroll(e, e->csi.arg[0] - 1, e->csi.arg[1] - 1);
				tmoveato(e, 0, 0);
			}
			break;
		case 's': /* DECSC -- Save cursor position (ANSI.SYS) */
			tcursor(e, CURSOR_SAVE);
			break;
		case 'u': /* DECRC -- Restore cursor position (ANSI.SYS) */
			if (e->csi.priv)
			{
				goto unknown;
			}
			else
			{
				tcursor(e, CURSOR_LOAD);
			}
			break;
		case ' ':
			switch (e->csi.mode[1])
			{
				case 'q': /* DECSCUSR -- Set Cursor Style */
					if (cb_setcursor(e, e->csi.arg[0]))
						goto unknown;
					break;
				default:
					goto unknown;
			}
			break;
	}
}

void
csidump(Emulator *e)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC[");
	for (i = 0; i < e->csi.len; i++)
	{
		c = e->csi.buf[i] & 0xff;
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
csireset(Emulator *e)
{
	memset(&e->csi, 0, sizeof(e->csi));
}

void
osc_color_response(Emulator *e, int num, int index, int is_osc4)
{
	int n;
	char buf[32];
	unsigned char r, g, b;

	if (cb_getcolor(e, is_osc4 ? num : index, &r, &g, &b))
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
		ttywrite(e, buf, n, 1);
	}
}

void
strhandle(Emulator *e)
{
	char *p = NULL, *dec;
	int j, narg, par;
	const struct
	{
		int idx;
		char *str;
	} osc_table[] = {
	    {defaultfg, "foreground"}, {defaultbg, "background"}, {defaultcs, "cursor"}};

	e->term.esc &= ~(ESC_STR_END | ESC_STR);
	strparse(e);
	par = (narg = e->str.narg) ? atoi(e->str.args[0]) : 0;

	switch (e->str.type)
	{
		case ']': /* OSC -- Operating System Command */
			switch (par)
			{
				case 0:
					if (narg > 1)
					{
						cb_settitle(e, e->str.args[1]);
						cb_seticontitle(e, e->str.args[1]);
					}
					return;
				case 1:
					if (narg > 1)
						cb_seticontitle(e, e->str.args[1]);
					return;
				case 2:
					if (narg > 1)
						cb_settitle(e, e->str.args[1]);
					return;
				case 52: /* manipulate selection data */
					if (narg > 2 && allowwindowops)
					{
						dec = base64dec(e->str.args[2]);
						if (dec)
						{
							cb_setsel(e, dec);
							cb_clipcopy(e);
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
					p = e->str.args[1];
					if ((j = par - 10) < 0 || j >= LEN(osc_table))
						break; /* shouldn't be possible */

					if (!strcmp(p, "?"))
					{
						osc_color_response(e, par, osc_table[j].idx, 0);
					}
					else if (cb_setcolorname(e, osc_table[j].idx, p))
					{
						fprintf(stderr, "erresc: invalid %s color: %s\n",
						    osc_table[j].str, p);
					}
					else
					{
						tfulldirt(e);
					}
					return;
				case 4: /* color set */
					if (narg < 3)
						break;
					p = e->str.args[2];
					/* FALLTHROUGH */
				case 104: /* color reset */
					j = (narg > 1) ? atoi(e->str.args[1]) : -1;

					if (p && !strcmp(p, "?"))
					{
						osc_color_response(e, j, 0, 1);
					}
					else if (cb_setcolorname(e, j, p))
					{
						if (par == 104 && narg <= 1)
						{
							cb_loadcols(e);
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
						tfulldirt(e);
					}
					return;
				case 110: /* reset dynamic VT100 text foreground color */
				case 111: /* reset dynamic VT100 text background color */
				case 112: /* reset dynamic text cursor color */
					if (narg != 1)
						break;
					if ((j = par - 110) < 0 || j >= LEN(osc_table))
						break; /* shouldn't be possible */
					if (cb_setcolorname(e, osc_table[j].idx, NULL))
					{
						fprintf(stderr, "erresc: %s color not found\n",
						    osc_table[j].str);
					}
					else
					{
						tfulldirt(e);
					}
					return;
			}
			break;
		case 'k': /* old title set compatibility */
			cb_settitle(e, e->str.args[0]);
			return;
		case 'P': /* DCS -- Device Control String */
		case '_': /* APC -- Application Program Command */
		case '^': /* PM -- Privacy Message */
			return;
	}

	fprintf(stderr, "erresc: unknown str ");
	strdump(e);
}

void
strparse(Emulator *e)
{
	int c;
	char *p = e->str.buf;

	e->str.narg = 0;
	e->str.buf[e->str.len] = '\0';

	if (*p == '\0')
		return;

	while (e->str.narg < STR_ARG_SIZ)
	{
		e->str.args[e->str.narg++] = p;
		while ((c = *p) != ';' && c != '\0')
			++p;
		if (c == '\0')
			return;
		*p++ = '\0';
	}
}

void
strdump(Emulator *e)
{
	size_t i;
	uint c;

	fprintf(stderr, "ESC%c", e->str.type);
	for (i = 0; i < e->str.len; i++)
	{
		c = e->str.buf[i] & 0xff;
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
strreset(Emulator *e)
{
	e->str = (STREscape){
	    .buf = xrealloc(e->str.buf, STR_BUF_SIZ),
	    .siz = STR_BUF_SIZ,
	};
}

void
sendbreak(Emulator *e, const Arg *arg)
{
#ifdef _WIN32
	// ConPTY has no break equivalent; ignore.
	(void) arg;
#else
	if (tcsendbreak(e->cmdfd, 0))
		perror("Error sending break");
#endif
}

void
tprinter(Emulator *e, char *s, size_t len)
{
	if (e->iofd != -1 && xwrite(e->iofd, s, len) < 0)
	{
		perror("Error writing to output file");
#ifdef _WIN32
		_close(e->iofd);
#else
		close(e->iofd);
#endif
		e->iofd = -1;
	}
}

void
toggleprinter(Emulator *e, const Arg *arg)
{
	e->term.mode ^= MODE_PRINT;
}

void
printscreen(Emulator *e, const Arg *arg)
{
	tdump(e);
}

void
printsel(Emulator *e, const Arg *arg)
{
	tdumpsel(e);
}

void
tdumpsel(Emulator *e)
{
	char *ptr;

	if ((ptr = getsel(e)))
	{
		tprinter(e, ptr, strlen(ptr));
		free(ptr);
	}
}

void
tdumpline(Emulator *e, int n)
{
	char buf[UTF_SIZ];
	const Glyph *bp, *end;

	bp = &e->term.line[n][0];
	end = &bp[MIN(tlinelen(e, n), e->term.col) - 1];
	if (bp != end || bp->u != ' ')
	{
		for (; bp <= end; ++bp)
			tprinter(e, buf, utf8encode(bp->u, buf));
	}
	tprinter(e, "\n", 1);
}

void
tdump(Emulator *e)
{
	int i;

	for (i = 0; i < e->term.row; ++i)
		tdumpline(e, i);
}

void
tputtab(Emulator *e, int n)
{
	uint x = e->term.c.x;

	if (n > 0)
	{
		while (x < e->term.col && n--)
			for (++x; x < e->term.col && !e->term.tabs[x]; ++x)
				/* nothing */;
	}
	else if (n < 0)
	{
		while (x > 0 && n++)
			for (--x; x > 0 && !e->term.tabs[x]; --x)
				/* nothing */;
	}
	e->term.c.x = LIMIT(x, 0, e->term.col - 1);
}

void
tdefutf8(Emulator *e, char ascii)
{
	if (ascii == 'G')
		e->term.mode |= MODE_UTF8;
	else if (ascii == '@')
		e->term.mode &= ~MODE_UTF8;
}

void
tdeftran(Emulator *e, char ascii)
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
		e->term.trantbl[e->term.icharset] = vcs[p - cs];
	}
}

void
tdectest(Emulator *e, char c)
{
	int x, y;

	if (c == '8')
	{ /* DEC screen alignment test. */
		for (x = 0; x < e->term.col; ++x)
		{
			for (y = 0; y < e->term.row; ++y)
				tsetchar(e, 'E', &e->term.c.attr, x, y);
		}
	}
}

void
tstrsequence(Emulator *e, uchar c)
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
	strreset(e);
	e->str.type = c;
	e->term.esc |= ESC_STR;
}

void
tcontrolcode(Emulator *e, uchar ascii)
{
	switch (ascii)
	{
		case '\t': /* HT */
			tputtab(e, 1);
			return;
		case '\b': /* BS */
			tmoveto(e, e->term.c.x - 1, e->term.c.y);
			return;
		case '\r': /* CR */
			tmoveto(e, 0, e->term.c.y);
			return;
		case '\f': /* LF */
		case '\v': /* VT */
		case '\n': /* LF */
			// go to first col if the mode is set
			tnewline(e, IS_SET(e, MODE_CRLF));
			return;
		case '\a': /* BEL */
			if (e->term.esc & ESC_STR_END)
			{
				// backwards compatibility to xterm
				strhandle(e);
			}
			else
			{
				cb_bell(e);
			}
			break;
		case '\033': /* ESC */
			csireset(e);
			e->term.esc &= ~(ESC_CSI | ESC_ALTCHARSET | ESC_TEST);
			e->term.esc |= ESC_START;
			return;
		case '\016': /* SO (LS1 -- Locking shift 1) */
		case '\017': /* SI (LS0 -- Locking shift 0) */
			e->term.charset = 1 - (ascii - '\016');
			return;
		case '\032': /* SUB */
			tsetchar(e, '?', &e->term.c.attr, e->term.c.x, e->term.c.y);
			/* FALLTHROUGH */
		case '\030': /* CAN */
			csireset(e);
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
			tnewline(e, 1); /* always go to first col */
			break;
		case 0x86: /* TODO: SSA */
		case 0x87: /* TODO: ESA */
			break;
		case 0x88: /* HTS -- Horizontal tab stop */
			e->term.tabs[e->term.c.x] = 1;
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
			ttywrite(e, vtiden, strlen(vtiden), 0);
			break;
		case 0x9b: /* TODO: CSI */
		case 0x9c: /* TODO: ST */
			break;
		case 0x90: /* DCS -- Device Control String */
		case 0x9d: /* OSC -- Operating System Command */
		case 0x9e: /* PM -- Privacy Message */
		case 0x9f: /* APC -- Application Program Command */
			tstrsequence(e, ascii);
			return;
	}
	// only CAN, SUB, \a and C1 chars interrupt a sequence
	e->term.esc &= ~(ESC_STR_END | ESC_STR);
}

/*
   returns 1 when the sequence is finished and it hasn't to read
   more characters for this sequence, otherwise 0
*/
int
eschandle(Emulator *e, uchar ascii)
{
	switch (ascii)
	{
		case '[':
			e->term.esc |= ESC_CSI;
			return 0;
		case '#':
			e->term.esc |= ESC_TEST;
			return 0;
		case '%':
			e->term.esc |= ESC_UTF8;
			return 0;
		case 'P': /* DCS -- Device Control String */
		case '_': /* APC -- Application Program Command */
		case '^': /* PM -- Privacy Message */
		case ']': /* OSC -- Operating System Command */
		case 'k': /* old title set compatibility */
			tstrsequence(e, ascii);
			return 0;
		case 'n': /* LS2 -- Locking shift 2 */
		case 'o': /* LS3 -- Locking shift 3 */
			e->term.charset = 2 + (ascii - 'n');
			break;
		case '(': /* GZD4 -- set primary charset G0 */
		case ')': /* G1D4 -- set secondary charset G1 */
		case '*': /* G2D4 -- set tertiary charset G2 */
		case '+': /* G3D4 -- set quaternary charset G3 */
			e->term.icharset = ascii - '(';
			e->term.esc |= ESC_ALTCHARSET;
			return 0;
		case 'D': /* IND -- Linefeed */
			if (e->term.c.y == e->term.bot)
			{
				tscrollup(e, e->term.top, 1);
			}
			else
			{
				tmoveto(e, e->term.c.x, e->term.c.y + 1);
			}
			break;
		case 'E':		/* NEL -- Next line */
			tnewline(e, 1); /* always go to first col */
			break;
		case 'H': /* HTS -- Horizontal tab stop */
			e->term.tabs[e->term.c.x] = 1;
			break;
		case 'M': /* RI -- Reverse index */
			if (e->term.c.y == e->term.top)
			{
				tscrolldown(e, e->term.top, 1);
			}
			else
			{
				tmoveto(e, e->term.c.x, e->term.c.y - 1);
			}
			break;
		case 'Z': /* DECID -- Identify Terminal */
			ttywrite(e, vtiden, strlen(vtiden), 0);
			break;
		case 'c': /* RIS -- Reset to initial state */
			treset(e);
			resettitle(e);
			cb_loadcols(e);
			cb_setmode(e, 0, MODE_HIDE);
			cb_setmode(e, 0, MODE_BRCKTPASTE);
			break;
		case '=': /* DECPAM -- Application keypad */
			cb_setmode(e, 1, MODE_APPKEYPAD);
			break;
		case '>': /* DECPNM -- Normal keypad */
			cb_setmode(e, 0, MODE_APPKEYPAD);
			break;
		case '7': /* DECSC -- Save Cursor */
			tcursor(e, CURSOR_SAVE);
			break;
		case '8': /* DECRC -- Restore Cursor */
			tcursor(e, CURSOR_LOAD);
			break;
		case '\\': /* ST -- String Terminator */
			if (e->term.esc & ESC_STR_END)
				strhandle(e);
			break;
		default:
			fprintf(stderr, "erresc: unknown sequence ESC 0x%02X '%c'\n", (uchar) ascii,
			    isprint(ascii) ? ascii : '.');
			break;
	}
	return 1;
}

void
tputc(Emulator *e, Rune u)
{
	char c[UTF_SIZ];
	int control;
	int width, len;
	Glyph *gp;

	control = ISCONTROL(u);
	if (u < 127 || !IS_SET(e, MODE_UTF8))
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

	if (IS_SET(e, MODE_PRINT))
		tprinter(e, c, len);

	/*
	   STR sequence must be checked before anything else
	   because it uses all following characters until it
	   receives a ESC, a SUB, a ST or any other C1 control
	   character.
	*/
	if (e->term.esc & ESC_STR)
	{
		if (u == '\a' || u == 030 || u == 032 || u == 033 || ISCONTROLC1(u))
		{
			e->term.esc &= ~(ESC_START | ESC_STR);
			e->term.esc |= ESC_STR_END;
			goto check_control_code;
		}

		if (e->str.len + len >= e->str.siz)
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
			 * e->term.esc = 0;
			 * strhandle(e);
			 */
			if (e->str.siz > (SIZE_MAX - UTF_SIZ) / 2)
				return;
			e->str.siz *= 2;
			e->str.buf = xrealloc(e->str.buf, e->str.siz);
		}

		memmove(&e->str.buf[e->str.len], c, len);
		e->str.len += len;
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
		if (IS_SET(e, MODE_UTF8) && ISCONTROLC1(u))
			return;
		tcontrolcode(e, u);
		/*
		   control codes are not shown ever
		*/
		if (!e->term.esc)
			e->term.lastc = 0;
		return;
	}
	else if (e->term.esc & ESC_START)
	{
		if (e->term.esc & ESC_CSI)
		{
			e->csi.buf[e->csi.len++] = u;
			if (between(u, 0x40, 0x7E) || e->csi.len >= sizeof(e->csi.buf) - 1)
			{
				e->term.esc = 0;
				csiparse(e);
				csihandle(e);
			}
			return;
		}
		else if (e->term.esc & ESC_UTF8)
		{
			tdefutf8(e, u);
		}
		else if (e->term.esc & ESC_ALTCHARSET)
		{
			tdeftran(e, u);
		}
		else if (e->term.esc & ESC_TEST)
		{
			tdectest(e, u);
		}
		else
		{
			if (!eschandle(e, u))
				return;
			// sequence already finished
		}
		e->term.esc = 0;
		return;
	}
	if (selected(e, e->term.c.x, e->term.c.y))
		selclear(e);

	gp = &e->term.line[e->term.c.y][e->term.c.x];
	if (IS_SET(e, MODE_WRAP) && (e->term.c.state & CURSOR_WRAPNEXT))
	{
		gp->mode |= ATTR_WRAP;
		tnewline(e, 1);
		gp = &e->term.line[e->term.c.y][e->term.c.x];
	}

	if (IS_SET(e, MODE_INSERT) && e->term.c.x + width < e->term.col)
	{
		memmove(gp + width, gp, (e->term.col - e->term.c.x - width) * sizeof(Glyph));
		gp->mode &= ~ATTR_WIDE;
	}

	if (e->term.c.x + width > e->term.col)
	{
		if (IS_SET(e, MODE_WRAP))
			tnewline(e, 1);
		else
			tmoveto(e, e->term.col - width, e->term.c.y);
		gp = &e->term.line[e->term.c.y][e->term.c.x];
	}

	tsetchar(e, u, &e->term.c.attr, e->term.c.x, e->term.c.y);
	e->term.lastc = u;

	if (width == 2)
	{
		gp->mode |= ATTR_WIDE;
		if (e->term.c.x + 1 < e->term.col)
		{
			if (gp[1].mode == ATTR_WIDE && e->term.c.x + 2 < e->term.col)
			{
				gp[2].u = ' ';
				gp[2].mode &= ~ATTR_WDUMMY;
			}
			gp[1].u = '\0';
			gp[1].mode = ATTR_WDUMMY;
		}
	}
	if (e->term.c.x + width < e->term.col)
	{
		tmoveto(e, e->term.c.x + width, e->term.c.y);
	}
	else
	{
		e->term.c.state |= CURSOR_WRAPNEXT;
	}
}

int
twrite(Emulator *e, const char *buf, int buflen, int show_ctrl)
{
	int charsize;
	Rune u;
	int n;

	for (n = 0; n < buflen; n += charsize)
	{
		if (IS_SET(e, MODE_UTF8))
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
				tputc(e, '^');
				tputc(e, '[');
			}
			else if (u != '\n' && u != '\r' && u != '\t')
			{
				u ^= 0x40;
				tputc(e, '^');
			}
		}
		tputc(e, u);
	}
	return n;
}

void
tresize(Emulator *e, int col, int row)
{
	int i;
	int minrow = MIN(row, e->term.row);
	int mincol = MIN(col, e->term.col);
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
	for (i = 0; i <= e->term.c.y - row; i++)
	{
		free(e->term.line[i]);
		free(e->term.alt[i]);
	}
	// ensure that both src and dst are not NULL
	if (i > 0)
	{
		memmove(e->term.line, e->term.line + i, row * sizeof(Line));
		memmove(e->term.alt, e->term.alt + i, row * sizeof(Line));
	}
	for (i += row; i < e->term.row; i++)
	{
		free(e->term.line[i]);
		free(e->term.alt[i]);
	}

	// resize to new height
	e->term.line = xrealloc(e->term.line, row * sizeof(Line));
	e->term.alt = xrealloc(e->term.alt, row * sizeof(Line));
	e->term.dirty = xrealloc(e->term.dirty, row * sizeof(*e->term.dirty));
	e->term.tabs = xrealloc(e->term.tabs, col * sizeof(*e->term.tabs));

	// resize each row to new width, zero-pad if needed
	for (i = 0; i < minrow; i++)
	{
		e->term.line[i] = xrealloc(e->term.line[i], col * sizeof(Glyph));
		e->term.alt[i] = xrealloc(e->term.alt[i], col * sizeof(Glyph));
	}

	// allocate any new rows
	for (/* i = minrow */; i < row; i++)
	{
		e->term.line[i] = xmalloc(col * sizeof(Glyph));
		e->term.alt[i] = xmalloc(col * sizeof(Glyph));
	}
	if (col > e->term.col)
	{
		bp = e->term.tabs + e->term.col;

		memset(bp, 0, sizeof(*e->term.tabs) * (col - e->term.col));
		while (--bp > e->term.tabs && !*bp)
			/* nothing */;
		for (bp += tabspaces; bp < e->term.tabs + col; bp += tabspaces)
			*bp = 1;
	}
	// update terminal size
	e->term.col = col;
	e->term.row = row;
	// reset scrolling region
	tsetscroll(e, 0, row - 1);
	// make use of the LIMIT in tmoveto
	tmoveto(e, e->term.c.x, e->term.c.y);
	// Clearing both screens (it makes dirty all lines)
	c = e->term.c;
	for (i = 0; i < 2; i++)
	{
		if (mincol < col && 0 < minrow)
		{
			tclearregion(e, mincol, 0, col - 1, minrow - 1);
		}
		if (0 < col && minrow < row)
		{
			tclearregion(e, 0, minrow, col - 1, row - 1);
		}
		tswapscreen(e);
		tcursor(e, CURSOR_LOAD);
	}
	e->term.c = c;
}

void
resettitle(Emulator *e)
{
	cb_settitle(e, NULL);
}

void
drawregion(Emulator *e, int x1, int y1, int x2, int y2)
{
	int y;

	for (y = y1; y < y2; y++)
	{
		if (!e->term.dirty[y])
			continue;

		e->term.dirty[y] = 0;
		cb_drawline(e, e->term.line[y], x1, y, x2);
	}
}

void
draw(Emulator *e)
{
	int cx = e->term.c.x, ocx = e->term.ocx, ocy = e->term.ocy;

	if (!cb_startdraw(e))
		return;

	// adjust cursor position
	LIMIT(e->term.ocx, 0, e->term.col - 1);
	LIMIT(e->term.ocy, 0, e->term.row - 1);
	if (e->term.line[e->term.ocy][e->term.ocx].mode & ATTR_WDUMMY)
		e->term.ocx--;
	if (e->term.line[e->term.c.y][cx].mode & ATTR_WDUMMY)
		cx--;

	drawregion(e, 0, 0, e->term.col, e->term.row);
	cb_drawcursor(e, cx, e->term.c.y, e->term.line[e->term.c.y][cx], e->term.ocx, e->term.ocy,
	    e->term.line[e->term.ocy][e->term.ocx]);
	e->term.ocx = cx;
	e->term.ocy = e->term.c.y;
	cb_finishdraw(e);
	if (ocx != e->term.ocx || ocy != e->term.ocy)
		cb_ximspot(e, e->term.ocx, e->term.ocy);
}

void
redraw(Emulator *e)
{
	tfulldirt(e);
	draw(e);
}
