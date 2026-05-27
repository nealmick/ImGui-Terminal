/* See LICENSE for license details. */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <wchar.h>

#ifdef _WIN32
#include <windows.h>
#ifdef _MSC_VER
typedef SSIZE_T ssize_t;
#endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

	/* Sizing constants for struct definitions below. */
	enum
	{
		UTF_SIZ = 4,
		ESC_BUF_SIZ = 128 * UTF_SIZ,
		ESC_ARG_SIZ = 16,
		STR_BUF_SIZ = ESC_BUF_SIZ,
		STR_ARG_SIZ = ESC_ARG_SIZ,
	};

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

	typedef struct Emulator
	{
		Term term;
		Selection sel;
		CSIEscape csi;
		STREscape str;

#ifndef _WIN32
		int iofd;
		int cmdfd;
		pid_t pid;
#else
	int iofd;
	int cmdfd; /* opaque on Windows; >= 0 marks the tty as open */
	HPCON w32_hpc;
	HANDLE w32_pipe_in;
	HANDLE w32_pipe_out;
	HANDLE w32_proc;
	HANDLE w32_reader_ready;
	int w32_reader_signaled; /* was function-local static in ttyread */
#endif

		char read_buf[BUFSIZ]; /* was function-local static in ttyread */
		int read_buflen;

		TCursor saved_cursor[2]; /* was function-local static in tcursor */

		int alive; /* 1 = child running, 0 after ttyread EOF */

		void *host; /* opaque back-pointer to owning Terminal */
	} Emulator;

	/* Public emulator API. Every function takes Emulator *e as first arg. */
	void tnew(Emulator *e, int cols, int rows);
	void tresize(Emulator *e, int, int);
	void tsetdirtattr(Emulator *e, int);
	int tattrset(Emulator *e, int);
	size_t ttyread(Emulator *e);
	void ttywrite(Emulator *e, const char *, size_t, int);
	void ttyresize(Emulator *e, int, int);
	void ttyhangup(Emulator *e);
	int ttynew(Emulator *e, const char *, char *, const char *, char **);
	void resettitle(Emulator *e);
	void redraw(Emulator *e);
	void draw(Emulator *e);
	void selinit(Emulator *e);
	void selclear(Emulator *e);
	void selstart(Emulator *e, int, int, int);
	void selextend(Emulator *e, int, int, int, int);
	int selected(Emulator *e, int, int);
	char *getsel(Emulator *e);
	void printscreen(Emulator *e, const Arg *);
	void printsel(Emulator *e, const Arg *);
	void sendbreak(Emulator *e, const Arg *);
	void toggleprinter(Emulator *e, const Arg *);

	/* Stateless helpers, no Emulator arg. */
	void *xmalloc(size_t);
	void *xrealloc(void *, size_t);
	char *xstrdup(const char *);
	size_t utf8encode(Rune, char *);
	void die(const char *, ...);

	/* config globals defined in imgui_terminal.cpp */
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

#ifdef __cplusplus
}
#endif
