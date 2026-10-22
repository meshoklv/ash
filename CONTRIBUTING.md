# Contributing

Issues and pull requests are welcome.

- Build first (`README.md`, Quick start, step 1). Keep simulation modules free of PSP headers so they still run natively.
- Say what you tested on: real PSP model + firmware, or emulator only. Emulator FPS is not hardware FPS.
- **Never** attach or commit game files, cooked `.ash*` data, extracted tables, or screenshots/videos of the game. Pull requests containing them will be closed.
- Do not add copy-protection bypasses, ROMs or save editors.
- For a crash on hardware, include `log.txt` and the decoded `crash.txt` (`tools/crash_decode.py`).
