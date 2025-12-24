# Code Analysis for `editor_min.c`

This document separates the user-defined functions (code you created) from the external library/system functions used in `editor_min.c`. Functions are listed with their purposes and, where applicable, line numbers for user-defined ones.

## User-Defined Functions

These are functions implemented in `editor_min.c` (your project code). Line numbers indicate the approximate start of the function definition.

```
- detect_language — L88: Detects programming language based on file extension.
- detect_tool — L101: Detects build tools or compilers available.
- parse_flags_from_help — L113: Parses command-line flags from help output.
- handle_include_update_signal — L274: Handles signals for include updates.
- free_tab — L276: Frees memory for a tab structure.
- make_tab_from_editor — L283: Creates a tab from the current editor state.
- append_tab — L292: Appends a tab to the tab list.
- push_current_to_tab_list — L299: Pushes current editor to tab list.
- insert_tab_at — L306: Inserts a tab at a specific position.
- push_home_to_front — L322: Pushes home tab to front.
- switch_to_tab — L340: Switches to a specific tab.
- save_current_to_home — L372: Saves current state to home tab.
- load_tab_snapshot — L378: Loads a snapshot for a tab.
- editorSeekLine — L390: Seeks to a specific line in the editor.
- close_tab — L398: Closes a tab.
- switch_to_home — L428: Switches to the home tab.
- die — L458: Handles fatal errors and exits.
- set_language_from_filename — L478: Sets language based on filename.
- run_embedded_terminal — L517: Runs an embedded terminal session.
- run_command_in_pty — L620: Runs a command in a PTY.
- disableRawMode — L727: Disables raw terminal mode.
- handle_terminate_signal — L750: Handles termination signals.
- handle_sigwinch — L761: Handles window resize signals.
- enableRawMode — L763: Enables raw terminal mode.
- getWindowSize — L785: Gets the terminal window size.
- editorAppendRow — L796: Appends a row to the editor buffer.
- editorInsertRow — L806: Inserts a row into the buffer.
- editorFreeRows — L818: Frees rows from memory.
- serialize_buffer — L826: Serializes the buffer for saving.
- is_unsaved — L843: Checks if the buffer has unsaved changes.
- load_config — L861: Loads editor configuration.
- compute_leading_spaces — L899: Computes leading spaces for indentation.
- make_indent_string — L910: Creates an indentation string.
- load_buffer_from_string — L932: Loads buffer from a string.
- save_snapshot — L975: Saves a snapshot of the editor state.
- now_ms — L1005: Gets current time in milliseconds.
- begin_batch — L1010: Begins a batch operation.
- end_batch — L1011: Ends a batch operation.
- maybe_snapshot — L1013: Conditionally saves a snapshot.
- load_snapshot — L1058: Loads a snapshot.
- undo_cmd — L1066: Performs undo operation.
- redo_cmd — L1077: Performs redo operation.
- delete_line_at — L1095: Deletes a line at a position.
- copy_line_n — L1106: Copies a specific line.
- copy_lines_list — L1115: Copies a list of lines.
- delete_last_words — L1141: Deletes last words in a line.
- delete_first_words — L1167: Deletes first words in a line.
- paste_at_line_n — L1191: Pastes at a specific line number.
- paste_at_line_n_front — L1304: Pastes at the front of a line.
- paste_at_cursor — L1417: Pastes at the cursor position.
- parse_number_at — L1477: Parses a number at a position.
- parse_line_list — L1493: Parses a list of lines.
- cmp_desc — L1523: Compares descriptions.
- save_to_file — L1527: Saves the buffer to a file.
- execute_command — L1570: Executes an editor command.
- editorInsertChar — L2748: Inserts a character into the buffer.
- editorInsertNewline — L2775: Inserts a newline.
- editorDelChar — L2833: Deletes a character.
- readKey — L2863: Reads a key press.
- editorScroll — L2917: Handles editor scrolling.
- draw_help_overlay — L2941: Draws the help overlay.
- visible_len — L3043: Calculates visible length of text.
- draw_welcome_overlay — L3054: Draws the welcome overlay.
- alphasort_entries — L3114: Sorts directory entries alphabetically.
- dir_entries — L3130: Gets directory entries.
- recursive_dir_entries — L3156: Gets recursive directory entries.
- fuzzy_score — L3202: Calculates fuzzy match score.
- match_cmp — L3235: Compares matches.
- collect_dir_paths — L3246: Collects directory paths.
- get_fuzzy_matches — L3284: Gets fuzzy matches for search.
- free_matches — L3369: Frees match structures.
- run_search — L3377: Runs a search operation.
- open_file_and_seek — L3485: Opens a file and seeks to a line.
- clear_active_suggestions — L3513: Clears active suggestions.
- update_suggestions_from_command — L3519: Updates suggestions from command.
- render_suggestions_overlay — L3605: Renders suggestions overlay.
- accept_active_suggestion — L3701: Accepts the active suggestion.
- pick_from_matches — L3837: Picks from matches.
- pick_multi_from_matches_with_desc — L3920: Picks multiple matches with descriptions.
- open_file_in_current_window_no_push — L3968: Opens file in current window without pushing to tabs.
- open_file_in_new_tab — L3989: Opens file in a new tab.
- detect_build_system — L4013: Detects the build system.
- get_make_targets — L4023: Gets make targets.
- prompt_input — L4045: Prompts for user input.
- prompt_yesno — L4062: Prompts for yes/no.
- free_string_array — L4070: Frees a string array.
- json_get_str — L4081: Gets string from JSON.
- load_build_config — L4097: Loads build configuration.
- save_build_config — L4113: Saves build configuration.
- find_in_path — L4129: Finds executable in PATH.
- detect_compiler — L4147: Detects available compiler.
- probe_compile_check — L4168: Probes compile check.
- probe_flag_supported — L4177: Probes if a flag is supported.
- detect_supported_standards — L4188: Detects supported standards.
- output_clear — L4206: Clears output.
- colorize_line_for_display — L4219: Colorizes a line for display.
- output_append_colored_line — L4233: Appends colored line to output.
- output_load_last_log — L4261: Loads last log.
- output_write_raw_to_log — L4283: Writes raw to log.
- strip_ansi — L4297: Strips ANSI codes.
- parse_error_location — L4324: Parses error location.
- interactive_setup — L4388: Runs interactive setup.
- tool_available — L4640: Checks if tool is available.
- run_interactive — L4647: Runs interactive mode.
- handle_build_run — L4849: Handles build and run.
- handle_run_only — L5014: Handles run only.
- show_file_browser — L5128: Shows file browser.
- editorRefreshScreen — L5548: Refreshes the editor screen.
- editorProcessKeypress — L5934: Processes key presses.
- initEditor — L6496: Initializes the editor.
```

## Library Functions

These are external functions from standard libraries or POSIX APIs, grouped by header/library. Each includes a brief explanation of its usage in the editor.

### `<stdlib.h>` (System Library - Standard Library - General Utilities)
- `malloc`: Allocates memory for editor rows, buffers, snapshots, undo history, and other dynamic data structures.
- `free`: Deallocates memory previously allocated with malloc to prevent memory leaks.
- `realloc`: Resizes allocated memory blocks, used for growing buffers or arrays like rows or matches.
- `atoi`: Converts string to integer, used for parsing numeric arguments in commands or configurations.
- `getenv`: Retrieves environment variables, used to check for editor settings or paths.
- `system`: Executes shell commands, used as a fallback or helper for running external commands.
- `_exit`: Terminates the process immediately, used in child processes after exec failures.
- `qsort`: Sorts arrays, used for sorting file lists in the file browser or matches in search.

### `<string.h>` (System Library - String Handling)
- `strcpy`: Copies strings, used for duplicating file paths, commands, or buffer contents.
- `strncpy`: Copies strings with length limit, used safely for buffer operations to avoid overflows.
- `strcat`: Concatenates strings, used for building command lines or paths.
- `strcmp`: Compares strings, used for checking file extensions, commands, or configuration keys.
- `strcasecmp`: Case-insensitive string comparison, used for language detection or command matching.
- `strlen`: Gets string length, used extensively for buffer sizing and parsing.
- `memcpy`: Copies memory blocks, used for efficient data transfer in buffers or rows.
- `memmove`: Moves memory blocks (handles overlaps), used for inserting/deleting text in buffers.
- `memset`: Sets memory to a value, used for initializing buffers or clearing data.
- `strchr`: Finds character in string, used for parsing commands or file paths.
- `strrchr`: Finds last occurrence of character, used for extracting file extensions or directories.
- `strstr`: Finds substring, used in search functionality within the editor.
- `strdup`: Duplicates string with malloc, used for storing persistent copies of strings like file names.
- `strndup`: Duplicates string with length limit, used safely for substring operations.

### `<stdio.h>` (System Library - Standard Input/Output)
- `printf`: Prints formatted output to console, used for debugging or status messages.
- `fprintf`: Prints to file or stream, used for logging to files or stderr.
- `snprintf`: Formats string safely with buffer limit, used for building status lines or commands.
- `sprintf`: Formats string (less safe), used in some formatting operations.
- `fopen`: Opens file for reading/writing, used for loading/saving files, configs, or swap files.
- `fclose`: Closes file stream, ensures data is flushed and resources freed.
- `fread`: Reads data from file, used for loading file contents into buffers.
- `fwrite`: Writes data to file, used for saving buffers to disk.
- `fgets`: Reads line from file, used for reading config files or subprocess output.
- `fdopen`: Opens file descriptor as stream, used for handling PTY output as file stream.
- `fflush`: Flushes output buffer, ensures immediate writing to files or console.
- `fsync`: Synchronizes file to disk, used for ensuring swap files are written durably.
- `popen`: Opens pipe to command, used for running commands and capturing output.
- `pclose`: Closes pipe, waits for command completion.

### `<unistd.h>` (System Library - POSIX - System Calls)
- `fork`: Creates child process, used for spawning PTY sessions or running commands.
- `execlp`: Executes program with path search, used in child processes to run shell commands.
- `execvp`: Executes program with arguments array, used for running build/run commands.
- `chdir`: Changes current directory, used for file browser navigation.
- `getcwd`: Gets current working directory, used for displaying paths or resolving relative paths.
- `access`: Checks file permissions, used for validating file existence or readability.
- `isatty`: Checks if file descriptor is a terminal, used for determining output mode.
- `write`: Writes to file descriptor, used for sending data to PTY or terminal.
- `read`: Reads from file descriptor, used for receiving input from PTY or stdin.
- `close`: Closes file descriptor, frees resources for files or sockets.
- `dup2`: Duplicates file descriptor, used for redirecting stdin/stdout in PTY setup.
- `pipe`: Creates pipe for inter-process communication, used in command execution.

### `<sys/ioctl.h>` (System Library - POSIX - I/O Control)
- `ioctl`: Performs I/O control operations, used for getting/setting terminal window size (TIOCGWINSZ/TIOCSWINSZ).

### `<termios.h>` (System Library - POSIX - Terminal I/O)
- `tcgetattr`: Gets terminal attributes, used for saving/restoring terminal state.
- `tcsetattr`: Sets terminal attributes, used for enabling raw mode or restoring cooked mode.
- `tcflush`: Flushes terminal input/output, used for clearing buffers during mode changes.

### `<sys/wait.h>` (System Library - POSIX - Process Waiting)
- `waitpid`: Waits for child process, used to monitor PTY processes or command executions.

### `<signal.h>` (System Library - POSIX - Signals)
- `signal`: Sets signal handler, used for handling SIGWINCH (window resize) or cleanup signals.
- `sigaction`: Advanced signal handling, used for more robust signal setup.
- `raise`: Sends signal to self, used for triggering handlers.
- `kill`: Sends signal to process, used for terminating child processes.

### `<time.h>` (System Library - Time Functions)
- `time`: Gets current time, used for timestamps in logs or undo history.
- `localtime`: Converts time to local time structure, used for formatting dates.
- `gettimeofday`: Gets high-resolution time, used for timing operations or delays.

### `<dirent.h>` (System Library - Directory Operations)
- `opendir`: Opens directory for reading, used in file browser to list directory contents.
- `readdir`: Reads directory entry, used to iterate through files in directories.
- `closedir`: Closes directory stream, frees resources.

### `<sys/stat.h>` (System Library - File Status)
- `stat`: Gets file status, used for checking file types, sizes, or modification times.
- `fstat`: Gets file status by descriptor, used for PTY or file descriptors.
- `mkdir`: Creates directory, used for creating directories in file operations.
- `chmod`: Changes file permissions, used for setting executable permissions on scripts.
- `unlink`: Deletes file, used for removing temporary or swap files.
- `rename`: Renames/moves file, used for saving files with new names.

### `<fcntl.h>` (System Library - File Control)
- `open`: Opens file with flags, used for low-level file operations like swap files.

### `<sys/select.h>` (System Library - I/O Multiplexing)
- `select`: Waits for I/O on multiple descriptors, used for multiplexing PTY input/output with stdin.
- `FD_SET`: Manipulates file descriptor sets, used in select calls.
- `FD_ZERO`: Initializes file descriptor sets, used in select setup.

### `<errno.h>` (System Library - Error Handling)
- `errno`: Global error variable, checked after system calls to handle failures.
- `strerror`: Converts errno to string, used for error messages.

### `<ctype.h>` (System Library - Character Classification)
- `isdigit`: Checks if character is digit, used in parsing numbers.
- `isalpha`: Checks if character is letter, used in syntax highlighting or parsing.
- `isspace`: Checks if character is whitespace, used extensively in text processing.
- `tolower`: Converts to lowercase, used for case-insensitive operations.
- `toupper`: Converts to uppercase, rarely used.
- `isalnum`: Checks if alphanumeric, used in tokenization.

### `<util.h>` or `<pty.h>` (System Library - PTY Utilities)
- `forkpty`: Creates PTY and forks, used for setting up embedded terminal sessions.

**Notes:**
- User-defined functions are implemented in `editor_min.c`.
- All listed libraries are **System Libraries**: They are part of the standard C library (glibc on Linux) or POSIX APIs provided by the operating system. There are no 3rd party libraries used in this project (i.e., no external libraries installed separately from package managers or sources).
- System libraries come pre-installed with the OS or compiler and provide core functionality like memory management, I/O, and system calls.
- 3rd party libraries would be external dependencies (e.g., ncurses, SDL) that need separate installation, but none are present here.
- This separation helps distinguish your code from external dependencies.