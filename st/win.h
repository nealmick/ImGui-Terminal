/* See LICENSE for license details. */

enum win_mode {
	MODE_VISIBLE     = 1 << 0,
	MODE_FOCUSED     = 1 << 1,
	MODE_APPKEYPAD   = 1 << 2,
	MODE_MOUSEBTN    = 1 << 3,
	MODE_MOUSEMOTION = 1 << 4,
	MODE_REVERSE     = 1 << 5,
	MODE_KBDLOCK     = 1 << 6,
	MODE_HIDE        = 1 << 7,
	MODE_APPCURSOR   = 1 << 8,
	MODE_MOUSESGR    = 1 << 9,
	MODE_8BIT        = 1 << 10,
	MODE_BLINK       = 1 << 11,
	MODE_FBLINK      = 1 << 12,
	MODE_FOCUS       = 1 << 13,
	MODE_MOUSEX10    = 1 << 14,
	MODE_MOUSEMANY   = 1 << 15,
	MODE_BRCKTPASTE  = 1 << 16,
	MODE_NUMLOCK     = 1 << 17,
	MODE_MOUSE       = MODE_MOUSEBTN|MODE_MOUSEMOTION|MODE_MOUSEX10\
	                  |MODE_MOUSEMANY,
};

/*
	The 16 functions below are the windowing-adapter contract — st.c
	calls them by name, and the currently-linked adapter (x.c OR
	imgui_win.cpp) provides the implementations.

	x.c — each name below has a direct implementation.

	imgui_win.cpp — uses a one-line thunk indirection: each x<name>
	is a tiny extern "C" wrapper at the bottom of the file that
	forwards to its imw_<name> implementation defined further up.
	The split lets the imgui adapter use descriptive names
	(`imw_drawline`, etc.) while preserving the x* linker contract
	st.c expects. So if you're tracing a call through imgui_win.cpp,
	the chain is:

	    st.c calls xdrawline(...)
	      -> thunk at bottom of imgui_win.cpp: forwards to imw_drawline
	         -> imw_drawline (the real body, defined above the thunks)

	Compiler inlines the thunks at any optimization level.
*/

void xbell(void);
void xclipcopy(void);
void xdrawcursor(int, int, Glyph, int, int, Glyph);
void xdrawline(Line, int, int, int);
void xfinishdraw(void);
void xloadcols(void);
int xsetcolorname(int, const char *);
int xgetcolor(int, unsigned char *, unsigned char *, unsigned char *);
void xseticontitle(char *);
void xsettitle(char *);
int xsetcursor(int);
void xsetmode(int, unsigned int);
void xsetpointermotion(int);
void xsetsel(char *);
int xstartdraw(void);
void xximspot(int, int);
