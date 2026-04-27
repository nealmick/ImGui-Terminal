# tests

Manual test scripts. Run inside the terminal under test — output should
look identical under both the imgui adapter and upstream x.c. Any
divergence is either a bug or a documented deliberate split (see
`notes.md`). Cross-reference between adapters to spot regressions.

| Script | Verifies |
|---|---|
| `render-test.sh`  | 8-phase attribute pipeline — colors, bold/italic/faint, underline/strike, blink, REVERSE, truecolor |
| `char-test.sh`    | UTF-8, box-drawing alignment, CJK wide-char layout, color emoji |
| `cursor-test.sh`  | All 7 cursor styles × focused/unfocused |
| `title-test.sh`   | OSC 0/1/2 window-title sequences |
| `palette-test.sh` | OSC 4 / 104 runtime palette mutation, multiple color-spec formats |
| `mouse-test.sh`   | Mouse-reporting protocol bytes — VT200, motion, free-motion, SGR |

Each script has its own header comment with details on what it
exercises and what to look for. All are interactive — they prompt for
Enter between sections, and clean up terminal state on exit.
