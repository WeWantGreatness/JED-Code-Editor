# Development Roadmap — Packaging & Distribution

Purpose
-------
This document records practical next steps for making JED distributable like Vim (production-ready `.deb` package, CI builds, lintian checks, and optional cross-distro packaging).

See `FILES.md` for a friendly, plain-English summary of important files and where to edit them.

Planned features (short)
------------------------
Here are two small, user-facing ideas to keep in the roadmap (keep them simple):

1. Autosave (optional): periodically write to a swap/auto-save file when buffer changes. Make it optional and configurable.
2. Simple config file: a tiny `~/.jedrc` or `~/.config/jed/config` with plain `key = value` lines for a few options (e.g. `indent_size`, `use_tabs`, `auto_save_interval`, `show_line_numbers`). Keep parsing minimal so users don't have to learn a complex format.

(If you'd like, I can add a minimal example `~/.jedrc` and a tiny parser to get started.)

Additional planned features
---------------------------
1. **Change Directory in Browser Menu**: Add `cd` command to allow users to change the current directory within the file browser overlay. This would update the browser's root directory for navigation.

2. **Delete Folder**: Implement `del <path>` command to delete folders. Ensure confirmation prompts and handle permissions/errors gracefully.

3. **File Rename Functionality (`fn`)**:
   - Allow renaming files with `fn <new_name>`.
   - If the file is unnamed (new buffer), proceed with naming it.
   - If the file already has a name, prompt for confirmation before renaming.
   - If the new name already exists, notify the user and prevent overwrite unless confirmed.

4. **Create New File with Tab Options**:
   - Use `fn` to create a new file while on a current file.
   - Prompt the user to choose: open in current tab or new tab.
   - If current tab, follow the same prompting as opening a file into the current tab (e.g., save current changes if unsaved).

5. **Native Git Integration**:
   - Add basic git commands like `git push`, `git pull`, `git status`, etc., accessible within the editor.
   - Implement as editor commands (e.g., `:git push`, `:git pull`).
   - Provide feedback and handle common scenarios like conflicts or authentication.

6. **Open Man Pages**:
   - Add ability to open man pages directly within the editor or via a command (e.g., `:man <topic>`).
   - If not natively supported, integrate with the embedded terminal or external viewer.

7. **Move Files in Browser**:
   - Add `mv <path>` command to move files to a specified directory path.
   - In the browser overlay, use `m` to select a file for moving, then navigate to the target directory and press `m` again with confirmation to move the file there (similar to delete confirmation).

8. **Delete All Lines Command**:
   - Implement `dall` command to delete all lines of code in the current file, similar to how `dl` deletes the selected line.

Current state
-------------
- `Makefile` with `install` and `install-man` targets
- Minimal `man/jed.1`
- `debian/` skeleton (control, rules, install, changelog)
- GitHub Actions workflow to build `.deb` artifacts

High-level goals
----------------
1. Produce a **production-grade `.deb`** (correct metadata, docs, man page, lintian-clean).
2. Automate builds and releases via **GitHub Actions** (build .deb on tags and attach to releases).
3. Add **CI lint step** (run `lintian` on produced `.deb` and fail on important issues).
4. Optionally provide Snap/Flatpak/AppImage artifacts for broader distro coverage.

Concrete tasks
--------------
- [ ] Improve `debian/control` metadata: **Homepage**, long **Description**, accurate **Depends**/**Suggests**, **Vcs-Browser**.
- [ ] Expand `debian/copyright` with full license details and source attribution.
- [ ] Run `lintian` on a built `.deb`, gather warnings, and fix high-priority items (missing manpage, missing docs, wrong file modes, missing Maintainer scripts hints).
- [ ] Add `debian/postrm` to remove package-owned system directories on purge (implemented; supports `JED_TEST_ROOT` for local testing).
- [ ] Add maintainable rules: ensure man pages, docs, and examples are installed into `/usr/share/` locations.
- [ ] Add CI `lintian` step to `.github/workflows/build-deb.yml` and fail on important levels.
- [ ] Add `debian/source/lintian-overrides` if there are benign exceptions to ignore.
- [ ] Add a `make package` helper (done) and a `make release` GitHub Action that signs and attaches `.deb` on tag.
- [ ] Optional: add `fpm`-based `make package-all` to produce `.deb` and `.rpm` in one step.
- [ ] Optional: add Snap/Flatpak/AppImage flows (follow-up depending on interest).

Testing & verification
----------------------
- Build locally: `dpkg-buildpackage -b -us -uc` (requires build deps).
- Lint: `lintian <pkg>.deb` and read top warnings.
- Install to local prefix or disposable container and run smoke tests:
  - `jed --help`
  - `man jed`
  - `jed test.c` create/open/save/quit flows

Priority checklist (recommended order)
-------------------------------------
1. Run `lintian` on current `.deb` and fix top warnings. (High)
2. Improve `debian/control` metadata and `debian/copyright`. (Medium)
3. Add CI lint step and release-on-tag step. (Medium)
4. Optionally add multi-format packaging (`fpm`) and snaps. (Low)

References
----------
- Debian New Maintainers' Guide: https://www.debian.org/doc/manuals/maint-guide/
- Lintian: https://lintian.debian.org/
- Debhelper/dh sequencer: https://wiki.debian.org/Teams/DebianDeveloperTools/Debhelper

Next step (I can do now)
------------------------
If you want, I can run `lintian` on the current `.deb`, list the warnings, and fix the top-priority ones.


Editor known issues / work in progress
-------------------------------------
- Command parsing strictness: tokens that should be boundary-checked (e.g. `jel`, `jl`, `jfl`, `jml`, and single-letter commands) are still being hardened — extra punctuation or unexpected adjacent characters can cause commands to fire or behave oddly in some cases. This requires more robust tokenization and test coverage (work in progress).
- Horizontal scrolling: typing past the right edge didn’t scroll horizontally previously (content would be hidden). Implemented a `col_offset` horizontal scroll and ensured cursor movements and typing keep the cursor visible.
- Auto-indent: pressing Enter now copies the previous line indentation and increases indent after `{` or `:`; typing `}` at line-start auto-dents one level. This follows `indent_size` and `use_tabs` config (implemented).
- Quit prompt & transient messages: ensure prompts (save on quit, swap recovery) are shown cleanly below the status bar and are cleared reliably; minor formatting tweaks may still be needed.
- Config option: `show_tab_bar` (true|false) to enable or disable the visual tab bar beneath the status line.
- New feature: `:browse` command opens a keyboard-driven file browser overlay that lists directory entries with numeric selection; supports entering directories and opening files into the current window.

File browser navigation notes
---------------------------
- The `:browse` overlay is a single, in-place overlay and does not spawn additional windows or stack overlays.
- Type a number and press Enter to open a file or enter a directory; if you enter a directory the view updates to show that directory's contents.
- Selecting a file opens it into the current editor buffer **without** pushing the previous buffer to a new tab (non-destructive).
- Backspace (Back) deletes the currently-typed digits (if any) and acts as "go back" when the selection is empty; at the initial directory Backspace shows a hint ("Press q or Esc to cancel the browser") instead of cancelling immediately; use `q` or `Esc` to cancel.
- Use Page Up / Page Down (page) to change pages if the directory is larger than the overlay; `q` or `Esc` cancels the browser.

Tab commands
------------
- `tabs` shows a compact, numbered listing of open tabs.
- `tabN` or `tab N` switches to tab N (1-based). The space between `tab` and the number is optional.
- `tabcN` or `tabc N` closes tab N (1-based). The space between `tabc` and the number is optional.

