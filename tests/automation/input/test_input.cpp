/* See LICENSE for license details. */
/*
	tests/automation/input/test_input.cpp — headless harness that
	exercises the adapter's INPUT pipeline end-to-end:
	   ImGui event → imw_dispatch_keyboard → ttywrite → PTY → bash
	   → ttyread → parser → row cache → term_dump_json
	No bash script as child; bash runs interactive (`bash -i`) and the
	test drives it via injected ImGui keyboard events between real
	frames. Same dump format as the core test client.

	  Build:  `make test_input`  →  build/test_input
	  Usage:  ./build/test_input --events <events.txt> --output <state.json>

	events.txt grammar (one directive per line, '#' comments, blank
	lines ignored):

	  type <text>          one ImGui character event per char (Stage 3
	                       path — InputQueueCharacters). One char per
	                       frame so the PTY drains between keystrokes.

	  key <Name>           a key press: down on frame N, up on N+1.
	                       Routed through Stage 2 (kmap). Names match
	                       ImGuiKey_<Name> with the prefix stripped:
	                       Enter, Backspace, Tab, Escape, F1..F12,
	                       UpArrow / DownArrow / LeftArrow / RightArrow.

	  chord <Mods> <Key>   modifier(s) down, key down, key up, mods up
	                       across two frames. Mods is one or more of
	                       Ctrl, Shift, Alt, Super joined with '+'.
	                       Routed through Stage 1 (shortcuts) or 2b
	                       (Ctrl+letter C0 mapping) depending on the
	                       chord. Examples: "chord Ctrl C",
	                       "chord Ctrl+Shift V".

	  wait <frames>        advance N frames without injecting anything,
	                       letting bash respond to prior input.

	  clipboard <text>     ImGui::SetClipboardText(text). Use before a
	                       Stage 1 paste chord to prime the clipboard.
	                       Without a backend, ImGui keeps an internal
	                       buffer — deterministic, no OS interaction.

	The harness boots EXACTLY like the production binaries:
	term_init(80, 24, ["bash", "--noprofile", "--norc", "-i"]) — same
	fonts, same palette, same PTY path. A real frame loop drives
	term_draw_canvas() each iteration inside its own Begin/End with
	SetKeyboardFocusHere() before the call so the canvas takes ImGui
	keyboard focus without a real mouse click. Per-frame events from
	the schedule are injected into ImGui's IO before NewFrame.
*/

#include <errno.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>
#include <string>

#include "imgui.h"
#include "../test_setup.h"

extern "C" {
#include "st.h"
}

extern void term_init(int cols, int rows, char **argv);
extern void term_draw_canvas(void);
extern void term_dump_json(FILE *out);

/*  --- event schedule ----------------------------------------------- */

struct Event {
	enum Kind { KEY_DOWN, KEY_UP, CHAR, SET_CLIPBOARD };
	Kind         kind;
	ImGuiKey     key;   /*  KEY_*; chord-mods like ImGuiMod_Ctrl too  */
	unsigned int ch;    /*  CHAR  */
	std::string  text;  /*  SET_CLIPBOARD  */
};

static std::vector<std::vector<Event>> g_schedule;

static void
sched_at(int frame, Event ev)
{
	if ((int)g_schedule.size() <= frame)
		g_schedule.resize(frame + 1);
	g_schedule[frame].push_back(ev);
}

/*  --- key-name lookup --------------------------------------------- */

struct KeyName {
	const char *name;
	ImGuiKey    key;
};

static const KeyName key_table[] = {
	/*  Common control keys  */
	{ "Enter",      ImGuiKey_Enter      },
	{ "Return",     ImGuiKey_Enter      },
	{ "Backspace",  ImGuiKey_Backspace  },
	{ "Tab",        ImGuiKey_Tab        },
	{ "Escape",     ImGuiKey_Escape     },
	{ "Space",      ImGuiKey_Space      },
	{ "Delete",     ImGuiKey_Delete     },
	{ "Insert",     ImGuiKey_Insert     },
	{ "Home",       ImGuiKey_Home       },
	{ "End",        ImGuiKey_End        },
	{ "PageUp",     ImGuiKey_PageUp     },
	{ "PageDown",   ImGuiKey_PageDown   },

	/*  Arrows  */
	{ "Up",         ImGuiKey_UpArrow    },
	{ "Down",       ImGuiKey_DownArrow  },
	{ "Left",       ImGuiKey_LeftArrow  },
	{ "Right",      ImGuiKey_RightArrow },
	{ "UpArrow",    ImGuiKey_UpArrow    },
	{ "DownArrow",  ImGuiKey_DownArrow  },
	{ "LeftArrow",  ImGuiKey_LeftArrow  },
	{ "RightArrow", ImGuiKey_RightArrow },

	/*  Function keys  */
	{ "F1",  ImGuiKey_F1  }, { "F2",  ImGuiKey_F2  },
	{ "F3",  ImGuiKey_F3  }, { "F4",  ImGuiKey_F4  },
	{ "F5",  ImGuiKey_F5  }, { "F6",  ImGuiKey_F6  },
	{ "F7",  ImGuiKey_F7  }, { "F8",  ImGuiKey_F8  },
	{ "F9",  ImGuiKey_F9  }, { "F10", ImGuiKey_F10 },
	{ "F11", ImGuiKey_F11 }, { "F12", ImGuiKey_F12 },

	/*  Letters and digits — used for chords like Ctrl+C, Ctrl+Shift+V  */
	{ "A", ImGuiKey_A }, { "B", ImGuiKey_B }, { "C", ImGuiKey_C },
	{ "D", ImGuiKey_D }, { "E", ImGuiKey_E }, { "F", ImGuiKey_F },
	{ "G", ImGuiKey_G }, { "H", ImGuiKey_H }, { "I", ImGuiKey_I },
	{ "J", ImGuiKey_J }, { "K", ImGuiKey_K }, { "L", ImGuiKey_L },
	{ "M", ImGuiKey_M }, { "N", ImGuiKey_N }, { "O", ImGuiKey_O },
	{ "P", ImGuiKey_P }, { "Q", ImGuiKey_Q }, { "R", ImGuiKey_R },
	{ "S", ImGuiKey_S }, { "T", ImGuiKey_T }, { "U", ImGuiKey_U },
	{ "V", ImGuiKey_V }, { "W", ImGuiKey_W }, { "X", ImGuiKey_X },
	{ "Y", ImGuiKey_Y }, { "Z", ImGuiKey_Z },
};

static ImGuiKey
lookup_key(const char *name)
{
	for (size_t i = 0; i < sizeof key_table / sizeof key_table[0]; i++)
		if (strcasecmp(name, key_table[i].name) == 0)
			return key_table[i].key;
	return ImGuiKey_None;
}

/*
	Map a left/right physical modifier key to its corresponding
	ImGuiMod_* flag. Mirrors imgui's internal GetModForLRModKey. Real
	backends send BOTH the physical key event AND the ImGuiMod_* event
	because io.KeyCtrl/Shift/Alt/Super are derived solely from the
	ImGuiMod_* slots, not from L/R key state (see GetMergedModsFromKeys
	in imgui.cpp and UpdateKeyModifiers in imgui_impl_glfw.cpp). The
	harness must do the same to produce realistic io.KeyCtrl/etc. state
	at the dispatcher.
*/
static ImGuiKey
mod_for_lr_key(ImGuiKey k)
{
	if (k == ImGuiKey_LeftCtrl  || k == ImGuiKey_RightCtrl)  return ImGuiMod_Ctrl;
	if (k == ImGuiKey_LeftShift || k == ImGuiKey_RightShift) return ImGuiMod_Shift;
	if (k == ImGuiKey_LeftAlt   || k == ImGuiKey_RightAlt)   return ImGuiMod_Alt;
	if (k == ImGuiKey_LeftSuper || k == ImGuiKey_RightSuper) return ImGuiMod_Super;
	return ImGuiKey_None;
}

static ImGuiKey
lookup_mod(const char *name)
{
	/*  Real left-side modifier KEYS, not ImGuiMod_* flags. AddKeyEvent
	    on a flag doesn't flip io.KeyCtrl/Shift/etc; only physical key
	    events do.

	    Ctrl/Super swap on macOS: ImGui's ConfigMacOSXBehaviors (default
	    on macOS) swaps LeftCtrl↔LeftSuper internally so a Mac user
	    pressing Cmd produces io.KeyCtrl=true — Cmd plays the
	    "app shortcut" role on Mac that Ctrl plays elsewhere. To get
	    logical Ctrl in tests we inject the physical key whose
	    platform-translated form flips io.KeyCtrl: LeftSuper on macOS,
	    LeftCtrl elsewhere. The dispatcher sees the same io.KeyCtrl=true
	    state in either case — production behavior unchanged.  */
#if defined(__APPLE__)
	if (strcasecmp(name, "Ctrl")  == 0) return ImGuiKey_LeftSuper;
	if (strcasecmp(name, "Super") == 0) return ImGuiKey_LeftCtrl;
	if (strcasecmp(name, "Cmd")   == 0) return ImGuiKey_LeftCtrl;
#else
	if (strcasecmp(name, "Ctrl")  == 0) return ImGuiKey_LeftCtrl;
	if (strcasecmp(name, "Super") == 0) return ImGuiKey_LeftSuper;
	if (strcasecmp(name, "Cmd")   == 0) return ImGuiKey_LeftSuper;
#endif
	if (strcasecmp(name, "Shift") == 0) return ImGuiKey_LeftShift;
	if (strcasecmp(name, "Alt")   == 0) return ImGuiKey_LeftAlt;
	return ImGuiKey_None;
}

/*  --- events.txt parser ------------------------------------------- */

static int
parse_events(const char *path)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "test_input: fopen %s: %s\n", path, strerror(errno));
		return -1;
	}

	int frame = 0;
	char line[1024];
	int lineno = 0;
	while (fgets(line, sizeof line, f)) {
		lineno++;
		size_t n = strlen(line);
		while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r' || line[n-1] == ' '))
			line[--n] = '\0';

		char *p = line;
		while (*p == ' ' || *p == '\t') p++;
		if (*p == '\0' || *p == '#') continue;

		char *cmd = p;
		while (*p && *p != ' ' && *p != '\t') p++;
		if (*p) { *p++ = '\0'; while (*p == ' ' || *p == '\t') p++; }
		char *arg = p;

		if (strcmp(cmd, "type") == 0) {
			/*  Decode UTF-8 byte sequences to codepoints — ImGui's
			    AddInputCharacter takes a codepoint, not a byte.
			    Multi-byte sequences in events.txt (é, 中, etc.)
			    must collapse to one CHAR event per character. */
			const unsigned char *q = (const unsigned char *)arg;
			while (*q) {
				unsigned int cp = 0; int n = 0;
				if      ((q[0] & 0x80) == 0x00) { cp = q[0]; n = 1; }
				else if ((q[0] & 0xe0) == 0xc0) { cp = q[0] & 0x1f; n = 2; }
				else if ((q[0] & 0xf0) == 0xe0) { cp = q[0] & 0x0f; n = 3; }
				else if ((q[0] & 0xf8) == 0xf0) { cp = q[0] & 0x07; n = 4; }
				else { q++; continue; }  /*  invalid lead — skip  */
				for (int k = 1; k < n; k++) {
					if ((q[k] & 0xc0) != 0x80) { n = 0; break; }
					cp = (cp << 6) | (q[k] & 0x3f);
				}
				if (n == 0) { q++; continue; }
				Event e = {};
				e.kind = Event::CHAR;
				e.ch   = cp;
				sched_at(frame, e);
				frame++;
				q += n;
			}
		} else if (strcmp(cmd, "key") == 0) {
			ImGuiKey k = lookup_key(arg);
			if (k == ImGuiKey_None) {
				fprintf(stderr, "test_input: %s:%d: unknown key '%s'\n",
				    path, lineno, arg);
				fclose(f); return -1;
			}
			Event d = {}; d.kind = Event::KEY_DOWN; d.key = k;
			Event u = {}; u.kind = Event::KEY_UP;   u.key = k;
			sched_at(frame,     d);
			sched_at(frame + 1, u);
			frame += 2;
		} else if (strcmp(cmd, "chord") == 0) {
			char *modspec = arg;
			while (*p && *p != ' ' && *p != '\t') p++;
			if (*p) { *p++ = '\0'; while (*p == ' ' || *p == '\t') p++; }
			char *keyname = p;

			std::vector<ImGuiKey> mods;
			char *m = modspec;
			while (m && *m) {
				char *plus = strchr(m, '+');
				if (plus) *plus = '\0';
				ImGuiKey mk = lookup_mod(m);
				if (mk == ImGuiKey_None) {
					fprintf(stderr, "test_input: %s:%d: unknown modifier '%s'\n",
					    path, lineno, m);
					fclose(f); return -1;
				}
				mods.push_back(mk);
				m = plus ? plus + 1 : NULL;
			}
			ImGuiKey k = lookup_key(keyname);
			if (k == ImGuiKey_None) {
				fprintf(stderr, "test_input: %s:%d: unknown key '%s'\n",
				    path, lineno, keyname);
				fclose(f); return -1;
			}
			Event ev = {};
			/*  For each modifier, send BOTH the physical L/R key and
			    its ImGuiMod_* flag — same shape GLFW backend produces.
			    Without the ImGuiMod_* event, io.KeyCtrl/Shift/etc.
			    stays false and Stage 2b/Stage 1 never fire. */
			for (ImGuiKey mk : mods) {
				ev = {}; ev.kind = Event::KEY_DOWN; ev.key = mk;
				sched_at(frame, ev);
				ImGuiKey mflag = mod_for_lr_key(mk);
				if (mflag != ImGuiKey_None) {
					ev = {}; ev.kind = Event::KEY_DOWN; ev.key = mflag;
					sched_at(frame, ev);
				}
			}
			ev = {}; ev.kind = Event::KEY_DOWN; ev.key = k; sched_at(frame, ev);
			ev = {}; ev.kind = Event::KEY_UP;   ev.key = k; sched_at(frame + 1, ev);
			for (ImGuiKey mk : mods) {
				ev = {}; ev.kind = Event::KEY_UP; ev.key = mk;
				sched_at(frame + 1, ev);
				ImGuiKey mflag = mod_for_lr_key(mk);
				if (mflag != ImGuiKey_None) {
					ev = {}; ev.kind = Event::KEY_UP; ev.key = mflag;
					sched_at(frame + 1, ev);
				}
			}
			frame += 2;
		} else if (strcmp(cmd, "wait") == 0) {
			int n2 = atoi(arg);
			if (n2 < 0) n2 = 0;
			frame += n2;
		} else if (strcmp(cmd, "clipboard") == 0) {
			/*  Stage 1 paste tests need ImGui's clipboard primed
			    before the chord fires. With no backend, ImGui's
			    SetClipboardText writes an internal buffer that
			    GetClipboardText reads back — deterministic. */
			Event e = {};
			e.kind = Event::SET_CLIPBOARD;
			e.text = arg;
			sched_at(frame, e);
			frame++;
		} else {
			fprintf(stderr, "test_input: %s:%d: unknown directive '%s'\n",
			    path, lineno, cmd);
			fclose(f); return -1;
		}
	}
	fclose(f);
	return frame;
}

/*  --- frame loop --------------------------------------------------- */

static void
inject_for_frame(int frame)
{
	if (frame >= (int)g_schedule.size()) return;
	ImGuiIO &io = ImGui::GetIO();
	for (const Event &e : g_schedule[frame]) {
		switch (e.kind) {
		case Event::KEY_DOWN: io.AddKeyEvent(e.key, true);  break;
		case Event::KEY_UP:   io.AddKeyEvent(e.key, false); break;
		case Event::CHAR:     io.AddInputCharacter(e.ch);   break;
		case Event::SET_CLIPBOARD:
			ImGui::SetClipboardText(e.text.c_str());
			break;
		}
	}
}

/*  --- atexit dump ------------------------------------------------- */

static const char *g_output_path = NULL;

static void
dump_atexit(void)
{
	if (!g_output_path) return;
	redraw();
	FILE *f = fopen(g_output_path, "wb");
	if (!f) {
		fprintf(stderr, "test_input: fopen %s: %s\n",
		    g_output_path, strerror(errno));
		return;
	}
	term_dump_json(f);
	fclose(f);
}

/*  --- main --------------------------------------------------------- */

int
main(int argc, char **argv)
{
	const char *events_path = NULL;
	const char *output_path = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--events") && i + 1 < argc)
			events_path = argv[++i];
		else if (!strcmp(argv[i], "--output") && i + 1 < argc)
			output_path = argv[++i];
	}
	if (!events_path || !output_path) {
		fprintf(stderr,
		    "usage: %s --events <events.txt> --output <state.json>\n",
		    argv[0]);
		return 1;
	}

	int last_frame = parse_events(events_path);
	if (last_frame < 0) return 2;

	/*  Make output_path absolute before chdir, otherwise the
	    relative path the runner passed becomes invalid.  */
	static char output_abs[PATH_MAX];
	output_path = make_path_absolute(output_path, output_abs, sizeof output_abs);

	g_output_path = output_path;
	atexit(dump_atexit);

	/*  Env + cwd pin — must happen before term_init so the forked
	    bash child inherits them.  */
	setup_test_env();
	setlocale(LC_CTYPE, "");

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.DisplaySize = ImVec2(800, 600);
	io.DeltaTime   = 1.0f / 60.0f;

	/*  Bash interactive. --noprofile --norc skips /etc/profile and
	    ~/.bashrc; we'd already pinned PS1 via env but those files
	    can override it.  */
	char *bash_path = (char *)find_bash();
	char nop[]    = "--noprofile";
	char norc[]   = "--norc";
	char dash_i[] = "-i";
	char *child_argv[] = { bash_path, nop, norc, dash_i, NULL };
	term_init(80, 24, child_argv);

	/*  Force-bake the font atlas. With no renderer backend, ImGui
	    won't lazy-build it from NewFrame and asserts on first text
	    draw. GetTexDataAsRGBA32 triggers the bake; we discard the
	    pixels (we never render them).  */
	{
		unsigned char *pix; int w, h;
		io.Fonts->GetTexDataAsRGBA32(&pix, &w, &h);
	}

#ifndef _WIN32
	signal(SIGCHLD, SIG_IGN);
#endif

	/*  Run a few extra drain frames after the last scheduled event so
	    bash has time to echo the final keystroke before the dump.  */
	int total_frames = last_frame + 30;
	if (total_frames < 5) total_frames = 5;

	for (int frame = 0; frame < total_frames; frame++) {
		inject_for_frame(frame);
		ImGui::NewFrame();
		/*  Drive the canvas directly inside our own Begin/End so we
		    can SetKeyboardFocusHere() in the same window context — the
		    canvas's InvisibleButton is the next item submitted, so
		    it ends up keyboard-focused without a real mouse click.  */
		/*  Match the geometry term_draw_widget() used to create so the
		    expected.json snapshots stay valid: 800x500 with default
		    chrome (title bar takes ~21px → canvas is 800x459).  */
		ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
		ImGui::Begin("Terminal");
		ImGui::SetKeyboardFocusHere();
		term_draw_canvas();
		ImGui::End();
		ImGui::EndFrame();
#ifdef _WIN32
		Sleep(20);     /*  20ms — ConPTY + MSYS2 bash needs more time  */
#else
		usleep(2000);  /*  ~2ms — let bash echo back  */
#endif
	}

	/*  Final drain after the loop: keep reading until the PTY is
	    quiet for ~50ms. Mirrors the core client's child-exit drain
	    but we never quit bash so there's no EOF.  */
	for (int i = 0; i < 25; i++) {
#ifdef _WIN32
		Sleep(20);
#else
		usleep(2000);
#endif
		ImGui::NewFrame();
		/*  Match the geometry term_draw_widget() used to create so the
		    expected.json snapshots stay valid: 800x500 with default
		    chrome (title bar takes ~21px → canvas is 800x459).  */
		ImGui::SetNextWindowSize(ImVec2(800, 500), ImGuiCond_FirstUseEver);
		ImGui::Begin("Terminal");
		ImGui::SetKeyboardFocusHere();
		term_draw_canvas();
		ImGui::End();
		ImGui::EndFrame();
	}

	/*  dump_atexit fires on return — see atexit() at start of main.  */
	return 0;
}
