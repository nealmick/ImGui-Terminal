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
#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <io.h>
#include <direct.h>
#define access _access
#ifndef X_OK
#define X_OK   0   /* MinGW _access only supports existence (0) and write (2) */
#endif
#define chdir  _chdir
#define getcwd _getcwd
/* MSVC's case-insensitive string compare is _stricmp; POSIX is
   strcasecmp. MinGW provides strcasecmp via <strings.h>, but pinning
   the macro here keeps the test sources free of compiler ifdefs. */
#ifdef _MSC_VER
#define strcasecmp _stricmp
#endif
#ifndef PATH_MAX
#define PATH_MAX MAX_PATH
#endif
#endif

static inline const char *
find_bash(void)
{
	static char path[256];
#ifdef _WIN32
	/* On Windows, look for bash in MSYS2/Git paths. */
	const char *candidates[] = {
		"C:/msys64/usr/bin/bash.exe",
		"C:/Program Files/Git/bin/bash.exe",
		NULL,
	};
#else
	const char *candidates[] = {
		"/opt/homebrew/bin/bash",  /*  Apple Silicon brew  */
		"/usr/local/bin/bash",    /*  Intel brew + Linuxbrew  */
		"/bin/bash",              /*  system fallback (bash 3.2 on macOS)  */
		NULL,
	};
#endif
	for (int i = 0; candidates[i]; i++) {
		if (access(candidates[i], X_OK) == 0) {
			snprintf(path, sizeof path, "%s", candidates[i]);
			/*  fprintf(stderr, "test_setup: bash = %s\n", path);  */
			return path;
		}
	}
	fprintf(stderr, "test_setup: no bash found\n");
	exit(2);
}

static inline void
setup_test_env(void)
{
#ifdef _WIN32
	/* Windows: use GetTempPath + a fixed subdirectory. */
	static char homedir[MAX_PATH];
	DWORD len = GetTempPathA(MAX_PATH, homedir);
	if (len == 0 || len >= MAX_PATH) {
		fprintf(stderr, "test_setup: GetTempPath failed\n");
		exit(2);
	}
	strncat(homedir, "imgui_test", MAX_PATH - len - 1);
	CreateDirectoryA(homedir, NULL);  /* OK if already exists */

	_putenv_s("PATH",     "C:\\msys64\\usr\\bin;C:\\Windows\\System32");
	_putenv_s("HOME",     homedir);
	_putenv_s("PS1",      "$ ");
	_putenv_s("PS2",      "> ");
	_putenv_s("TERM",     "xterm-256color");
	_putenv_s("LANG",     "C.UTF-8");
	_putenv_s("LC_ALL",   "C.UTF-8");
	_putenv_s("TZ",       "UTC");
	_putenv_s("INPUTRC",  "NUL");
	_putenv_s("HISTFILE", "NUL");
#else
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
#endif

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
#ifdef _WIN32
	if (p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':')) {
#else
	if (p[0] == '/') {
#endif
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
