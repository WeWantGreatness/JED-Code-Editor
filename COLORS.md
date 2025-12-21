# Editor Color Palette

This file lists the ANSI color sequences and their meanings as used in `editor_min.c`.

## Primary colors used

- **Reset**: `\x1b[0m` — reset attributes
- **Bold White / Header**: `\x1b[1;97m` — headings & labels (e.g., "File", "Tab")
- **Cyan**: `\x1b[96m` — file names in prompts
- **Green**: `\x1b[92m` — positive actions and descriptive words (e.g., `o: current tab`)
- **Blue**: `\x1b[94m` — neutral actions and descriptive words (e.g., `t: new tab`)
- **Yellow**: `\x1b[93m` — tips, warnings, cancel actions and descriptive words (e.g., `c: cancel`)
- **Red**: `\x1b[91m` — destructive or negative actions (e.g., `n`, delete)
- **Plain White**: `\x1b[37m` — inline command text in overlays
- **Reverse / Invert**: `\x1b[7m` — small badge-style highlights (used in help bar)
- **Cursor control**: `\x1b[5 q` / `\x1b[6 q` — set cursor style (blinking/steady bar)

## Usage notes
- The code uses `\x1b[1;9Xm` for bold + color on key letters (e.g., `\x1b[1;92mo\x1b[0m`), to make the option letter stand out.
- `visible_len()` is used throughout to account for ANSI sequences when computing positions.

If you'd like an alternate color scheme (different primary colors or different emphasis), tell me which color codes you prefer and I will update the prompts consistently.