/* See LICENSE for license details. */
/*
	tests/automation/core/test_core.cpp — headless harness that drives a
	scripted bash child through the adapter and dumps the resulting
	render state. No window, no GPU, no ImGui frame loop.

	  Build:  `make test_core`  →  build/test_core
	  Usage:  ./build/test_core --input <script.sh> --output <state.json>

	What it does:
	  1. Creates an ImGui context (needed because term_init -> imw_load_fonts
	     calls ImGui::GetIO()). No backend, no NewFrame.
	  2. term_init(80, 24, [bash, "--noprofile", "--norc", input_path,
	     NULL]) — same init path the real binaries use, just with a
	     scripted child.
	  3. Overrides st.c's SIGCHLD handler with SIG_IGN. st.c installs a
	     handler in ttynew() that calls die() (= exit) when the child
	     dies; with SIG_IGN, the child becomes a zombie that's auto-
	     reaped, and the parent learns about it via PTY EOF on the next
	     ttyread() call. That's the desired path because EOF only comes
	     after all buffered child output has been read; SIGCHLD races
	     ahead of the buffered data.
	  4. Loops calling ttyread() — blocks until data; processes it through
	     st.c's parser; eventually hits EOF and ttyread()'s read==0 branch
	     calls exit(0).
	  5. atexit() handler fires: one final redraw() to populate
	     imw_row_ops with the post-processing state, then term_dump_json
	     writes the snapshot.

	The dump function reads ONLY adapter state (DrawOp cache, tw, title).
	No interpretation happens here — the harness just drives the parser
	and serializes what the adapter decided to render.
*/

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "../test_setup.h"

extern "C" {
#include "st.h"
}

extern void term_init(int cols, int rows, char **argv);
extern void term_dump_json(FILE *out);

static const char *g_output_path = NULL;

static void
dump_atexit(void)
{
	if (!g_output_path)
		return;
	/*  Final redraw() so imw_row_ops reflects the post-processing
	    state of Term, not whatever was cached partway through.  */
	redraw();
	FILE *f = fopen(g_output_path, "wb");
	if (!f) {
		fprintf(stderr, "test_core: fopen %s: %s\n",
		    g_output_path, strerror(errno));
		return;
	}
	term_dump_json(f);
	fclose(f);
}

int
main(int argc, char **argv)
{
	const char *input_path  = NULL;
	const char *output_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--input") && i + 1 < argc)
			input_path = argv[++i];
		else if (!strcmp(argv[i], "--output") && i + 1 < argc)
			output_path = argv[++i];
	}
	if (!input_path || !output_path) {
		fprintf(stderr,
		    "usage: %s --input <script.sh> --output <state.json>\n",
		    argv[0]);
		return 1;
	}

	/*  Both paths are passed in relative to the runner's cwd; chdir
	    inside setup_test_env would invalidate them. Resolve to
	    absolute first.  */
	static char input_abs[PATH_MAX], output_abs[PATH_MAX];
	input_path  = make_path_absolute(input_path,  input_abs,  sizeof input_abs);
	output_path = make_path_absolute(output_path, output_abs, sizeof output_abs);

	g_output_path = output_path;
	atexit(dump_atexit);

	/*  Pin env (PATH/HOME/PS1/TERM/locale/TZ/etc.) and chdir into a
	    fresh tempdir before term_init forks bash. Same setup as
	    test_input — keeps baselines stable across machines.  */
	setup_test_env();
	setlocale(LC_CTYPE, "");

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();

	char *bash_path = (char *)find_bash();
	char nop[]  = "--noprofile";
	char norc[] = "--norc";
	char *child_argv[] = { bash_path, nop, norc, (char *)input_path, NULL };

	term_init(80, 24, child_argv);

#ifndef _WIN32
	/*  Override st.c's SIGCHLD handler. See file header for why.  */
	signal(SIGCHLD, SIG_IGN);
#endif

	/*  Pump until ttyread() hits EOF and calls exit(0) — which fires
	    dump_atexit. This loop never returns normally.  */
	for (;;)
		ttyread();

	return 0;  /*  unreachable  */
}
