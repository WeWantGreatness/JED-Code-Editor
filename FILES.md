FILES — Friendly guide

This is a short, friendly overview of the most important files in the repo so you know what to edit and what to leave alone.

- editor_min.c — The main program. Handles input, the screen, commands, file open/save, and status bar. Edit this for features like autosave, commands (e.g., `fn`), and UI updates.
- Makefile — Build and install helpers. Edit this when adding new build steps or packaging helpers.
- man/jed.1 — Manual page. Update when you add or change user-facing commands.
- debian/ — Debian packaging metadata (control, rules, install, postrm). Change this only when you update packaging.
- .github/workflows/ — CI workflows (build and release automation). Add tests and lint steps here.
- examples/jedrc.example — Example config file for users. Nice to keep in sync with the parser.

If you want a small change, edit only `editor_min.c` and the man page; larger distribution changes belong in `debian/` and CI configs.

Happy hacking! 👋