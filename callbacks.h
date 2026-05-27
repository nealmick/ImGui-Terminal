#pragma once
#include "emulator.h"
#ifdef __cplusplus
extern "C"
{
#endif

	void cb_bell(Emulator *);
	void cb_clipcopy(Emulator *);
	void cb_die(Emulator *, const char *);
	void cb_drawcursor(Emulator *, int, int, Glyph, int, int, Glyph);
	void cb_drawline(Emulator *, Line, int, int, int);
	void cb_finishdraw(Emulator *);
	void cb_loadcols(Emulator *);
	int cb_setcolorname(Emulator *, int, const char *);
	int cb_getcolor(Emulator *, int, unsigned char *, unsigned char *, unsigned char *);
	void cb_seticontitle(Emulator *, char *);
	void cb_settitle(Emulator *, char *);
	int cb_setcursor(Emulator *, int);
	void cb_setmode(Emulator *, int, unsigned int);
	void cb_setpointermotion(Emulator *, int);
	void cb_setsel(Emulator *, char *);
	int cb_startdraw(Emulator *);
	void cb_ximspot(Emulator *, int, int);

#ifdef __cplusplus
}
#endif
