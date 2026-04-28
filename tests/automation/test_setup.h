/* See LICENSE for license details. */
/*
	tests/automation/test_setup.h — shared environment-pinning and
	bash-discovery helpers used by both test clients (test_core,
	test_input). Header-only; static inline so each translation unit
	gets its own copy.

	  find_bash()        — probes homebrew bash 5 paths, falls back to
	                        /bin/bash. Logs the chosen path to stderr.
	  setup_test_env()   — sets PATH/HOME/PS1/TERM/locale/TZ/etc. to
	                        deterministic values so test baselines are
	                        stable across machines. mkdtemps a HOME and
	                        chdirs into it. Tempdir is leaked (in /tmp;
	                        OS reaps periodically).
	  make_path_absolute(p, out, n) — convert a possibly-relative path
	                        to absolute via getcwd, writing into `out`.
	                        Call BEFORE setup_test_env, since chdir
	                        invalidates relative paths.
*/

#pragma once

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline const char *
find_bash(void)
{
	static char path[256];
	const char *candidates[] = {
		"/opt/homebrew/bin/bash",  /*  Apple Silicon brew  */
		"/usr/local/bin/bash",     /*  Intel brew + Linuxbrew  */
		"/bin/bash",               /*  system fallback (bash 3.2 on macOS)  */
		NULL,
	};
	for (int i = 0; candidates[i]; i++) {
		if (access(candidates[i], X_OK) == 0) {
			snprintf(path, sizeof path, "%s", candidates[i]);
			fprintf(stderr, "test_setup: bash = %s\n", path);
			return path;
		}
	}
	fprintf(stderr, "test_setup: no bash found\n");
	exit(2);
}

static inline void
setup_test_env(void)
{
	static char homedir[] = "/tmp/imgui_test.XXXXXX";
	if (!mkdtemp(homedir)) {
		fprintf(stderr, "test_setup: mkdtemp failed: %s\n",
		    strerror(errno));
		exit(2);
	}

	setenv("PATH",     "/usr/bin:/bin",                 1);
	setenv("HOME",     homedir,                         1);
	setenv("PS1",      "$ ",                            1);
	setenv("PS2",      "> ",                            1);
	setenv("TERM",     "xterm-256color",                1);
	setenv("LANG",     "C.UTF-8",                       1);
	setenv("LC_ALL",   "C.UTF-8",                       1);
	setenv("TZ",       "UTC",                           1);
	setenv("INPUTRC",  "/dev/null",                     1);
	setenv("HISTFILE", "/dev/null",                     1);
	setenv("BASH_SILENCE_DEPRECATION_WARNING", "1",     1);

	if (chdir(homedir) != 0) {
		fprintf(stderr, "test_setup: chdir %s: %s\n",
		    homedir, strerror(errno));
		exit(2);
	}
}

/*
	Convert a possibly-relative path to absolute using the current
	cwd. Caller must invoke this BEFORE setup_test_env() chdirs away.
*/
static inline const char *
make_path_absolute(const char *p, char *out, size_t n)
{
	if (p[0] == '/') {
		snprintf(out, n, "%s", p);
		return out;
	}
	char cwd[PATH_MAX];
	if (!getcwd(cwd, sizeof cwd)) {
		fprintf(stderr, "test_setup: getcwd: %s\n", strerror(errno));
		exit(2);
	}
	snprintf(out, n, "%s/%s", cwd, p);
	return out;
}
