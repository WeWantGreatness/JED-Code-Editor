#define _DEFAULT_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <sys/time.h>
#include <pty.h>
#include <signal.h>
#include <dirent.h>
#include <strings.h>  /* for strcasestr */

#define EDIT_KIND_NONE 0
#define EDIT_KIND_INSERT 1
#define EDIT_KIND_DELETE 2
#define EDIT_KIND_NEWLINE 3
#define EDIT_GROUP_MS 1200

#include <sys/stat.h>
#include <unistd.h>

/* Syntax highlighting and include classification */
#include "highlight.h"
#include "include_classifier.h"

// Language definitions for build/run
typedef enum {
    LANG_NONE = 0,
    LANG_C,
    LANG_CPP,
    LANG_PYTHON,
    LANG_JAVA,
    LANG_CSHARP,
    LANG_JAVASCRIPT,
    LANG_GO,
    LANG_RUST,
    LANG_ZIG,
    LANG_LUA,
    LANG_BASH,
    LANG_RUBY,
    LANG_PERL,
    LANG_PHP
} Language;

typedef struct {
    const char *name;
    const char **extensions;
    int ext_count;
    const char **build_tools;
    int build_tool_count;
    const char **run_tools;
    int run_tool_count;
    const char *build_template;
    const char *run_template;
    int has_flags;
} LangDef;

static const LangDef lang_defs[] = {
    { "C", (const char*[]){"c"}, 1, (const char*[]){"gcc", "clang", "cc"}, 3, NULL, 0, "%s %s -o %s %s", "./%s", 1 },
    { "C++", (const char*[]){"cpp", "cc", "cxx"}, 3, (const char*[]){"g++", "clang++", "c++"}, 3, NULL, 0, "%s %s -o %s %s", "./%s", 1 },
    { "Python", (const char*[]){"py"}, 1, NULL, 0, (const char*[]){"python3", "python"}, 2, "", "python3 %s", 0 },
    { "Java", (const char*[]){"java"}, 1, (const char*[]){"javac"}, 1, (const char*[]){"java"}, 1, "javac %s", "java %s", 1 },
    { "C#", (const char*[]){"cs"}, 1, (const char*[]){"mcs"}, 1, (const char*[]){"mono"}, 1, "mcs %s -out:%s", "mono %s", 1 },
    { "JavaScript", (const char*[]){"js"}, 1, NULL, 0, (const char*[]){"node"}, 1, "", "node %s", 0 },
    { "Go", (const char*[]){"go"}, 1, (const char*[]){"go"}, 1, NULL, 0, "go build %s -o %s %s", "./%s", 1 },
    // Note: Rust uses the template "rustc %s -o %s %s" (src, output, flags).
    // Be careful: generating build commands from generic templates must preserve
    // the expected argument order. A previous bug produced malformed commands
    // like "rustc rustc -o main" due to incorrect template parameter ordering.
    // If you add or change templates, add tests and ensure `interactive_setup`
    // formats the arguments correctly.
    { "Rust", (const char*[]){"rs"}, 1, (const char*[]){"rustc"}, 1, NULL, 0, "rustc %s -o %s %s", "./%s", 1 },
    { "Zig", (const char*[]){"zig"}, 1, (const char*[]){"zig"}, 1, NULL, 0, "zig build-exe %s -femit-bin=%s", "./%s", 1 },
    { "Lua", (const char*[]){"lua"}, 1, NULL, 0, (const char*[]){"lua"}, 1, "", "lua %s", 0 },
    { "Bash", (const char*[]){"sh"}, 1, NULL, 0, (const char*[]){"bash"}, 1, "", "bash %s", 0 },
    { "Ruby", (const char*[]){"rb"}, 1, NULL, 0, (const char*[]){"ruby"}, 1, "", "ruby %s", 0 },
    { "Perl", (const char*[]){"pl"}, 1, NULL, 0, (const char*[]){"perl"}, 1, "", "perl %s", 0 },
    { "PHP", (const char*[]){"php"}, 1, NULL, 0, (const char*[]){"php"}, 1, "", "php %s", 0 }
};

static Language detect_language(const char *filename) {
    if (!filename) return LANG_NONE;
    const char *ext = strrchr(filename, '.');
    if (!ext) return LANG_NONE;
    ext++; // skip '.'
    for (int i = 0; i < sizeof(lang_defs)/sizeof(lang_defs[0]); i++) {
        for (int j = 0; j < lang_defs[i].ext_count; j++) {
            if (strcmp(ext, lang_defs[i].extensions[j]) == 0) return (Language)(i + 1);
        }
    }
    return LANG_NONE;
}

static int detect_tool(const char **tools, int count, char *found_tool, int tool_len) {
    for (int i = 0; i < count; i++) {
        char cmd[512]; snprintf(cmd, sizeof(cmd), "which %s >/dev/null 2>&1", tools[i]);
        if (system(cmd) == 0) {
            strncpy(found_tool, tools[i], tool_len - 1);
            found_tool[tool_len - 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static int parse_flags_from_help(const char *tool, char ***flags_out, int *count_out) {
    *flags_out = NULL;
    *count_out = 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s --help 2>&1 | head -50", tool);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    char line[1024];
    while (fgets(line, sizeof(line), fp) && *count_out < 50) {
        char *p = line;
        while (*p) {
            if (*p == '-' && (isalnum(p[1]) || p[1] == '-')) {
                char flag[64] = {0};
                int i = 0;
                while (*p && !isspace(*p) && *p != ',' && *p != '|' && i < 63) {
                    flag[i++] = *p++;
                }
                flag[i] = '\0';
                if (strlen(flag) > 1) {
                    int have = 0;
                    for (int j = 0; j < *count_out; j++) {
                        if (strcmp((*flags_out)[j], flag) == 0) {
                            have = 1;
                            break;
                        }
                    }
                    if (!have) {
                        char **n = realloc(*flags_out, sizeof(char*) * (*count_out + 1));
                        if (n) {
                            *flags_out = n;
                            (*flags_out)[*count_out] = strdup(flag);
                            (*count_out)++;
                        }
                    }
                }
            } else {
                p++;
            }
        }
    }
    pclose(fp);
    return *count_out > 0;
}

// Special keys
#define ARROW_LEFT 1000
#define ARROW_RIGHT 1001
#define ARROW_UP 1002
#define ARROW_DOWN 1003
#define HOME_KEY 1004
#define END_KEY 1005
#define PAGE_UP 1006
#define PAGE_DOWN 1007
#define DEL_KEY 1008

/* Minimal, robust line-based editor: typing characters, Enter to newline,
 * backspace, cursor, safe allocations. Designed to be small and easy to
 * reason about (no mappings/undo/history). */

typedef enum { MODE_VIEW = 0, MODE_EDIT = 1 } EditorMode;

#define HISTORY_LIMIT 100

struct Editor {
    int cx, cy;         // cursor column, row
    int numrows;
    char **rows;        // dynamic array of NUL-terminated strings
    struct termios orig_termios;
    int screenrows, screencols;
    int row_offset;     // first row shown in the window (for vertical scrolling)
    int col_offset;     // first column shown for horizontal scrolling

    EditorMode mode;    // view/edit
    char command_buf[256]; // command buffer for view mode
    int command_len;
    int help_visible;   // help overlay flag
    int help_scroll;    // scroll offset for help overlay
    int help_from_welcome; // flag if help was opened from welcome
    int browser_from_welcome; // flag if browser was opened from welcome
    int welcome_visible; // welcome/title screen visible flag

    int prompt_visible; // prompt overlay visible flag
    char prompt_message[1024];
    char prompt_options[256];
    char pending_path[512];
    int pending_lineno;

    // Output pane (build/run logs)
    int output_visible;       // whether output pane is visible
    char **output_lines;      // NULL-terminated list of colorized lines (allocated)
    char **output_raw_lines;  // raw lines for parsing
    int output_n;             // number of lines
    int output_scroll;        // top line index shown
    int output_sel;           // selected line index
    time_t output_last_mtime; // mtime of last loaded config/last_build.log


    // history snapshots
    char **history;
    int history_count;
    int history_idx;

    // clipboard
    char *clipboard;
    // batch mode counter: when >0, mutating ops shouldn't create snapshots per-char
    int batching;
    // last saved snapshot (for detecting unsaved changes)
    char *saved_snapshot;

    // filename (if any)
    char *filename;

    // last edit grouping
    int last_edit_kind;            // EDIT_KIND_*
    long long last_edit_time_ms;   // monotonic ms timestamp


    // transient message
    char msg[4096];
    time_t msg_time;

    // build/run flow helpers
    int awaiting_buildfile_save; // set when we open buildfile for edit and want to auto-run
    char post_edit_cmd[512]; // command to execute after saving the buildfile
} E;

// Simple tab abstraction: each tab stores a serialized snapshot of the buffer and cursor/filename
struct Tab {
    char *name; // filename or empty
    char *snapshot; // serialized buffer (caller must free)
    char *saved_snapshot; // serialized saved snapshot for this tab (NULL if unsaved/unknown)
    int cx, cy, row_offset, col_offset;
};
static struct Tab *Tabs = NULL;
static int NumTabs = 0;
#define MAX_TABS 10
static int CurTab = -1; // index of currently loaded tab (-1 == "home" / current buffer)

// Home tab snapshot: stores the "current buffer" state when the user switches away
static struct Tab HomeTab = {0};
static int HaveHomeTab = 0;

// Forward declarations for functions used by tab code (declared later)
static char *serialize_buffer(void);
static void editorFreeRows(void);
static void load_buffer_from_string(const char *s);
static void editorScroll(void);
static void getWindowSize(void);
// Output pane forward declarations
static void output_append_line(const char *line);
static void output_clear(void);
static char *colorize_line_for_display(const char *line);
static void output_append_colored_line(const char *rawline);
static void output_clear(void);
static int parse_error_location(const char *line, char *path, int pathlen, int *lineno, int *colno);
/* Forward declarations for Build/Run handlers (used by command parsing before definition) */
static void handle_build_run(int do_run_after);
static void handle_run_only(int allow_interactive, const char *arg);
static volatile sig_atomic_t winch_received = 0;
static volatile sig_atomic_t include_update_received = 0;

static void handle_include_update_signal(int sig) { (void)sig; include_update_received = 1; }

static void free_tab(struct Tab *t) {
    if (!t) return;
    free(t->name); free(t->snapshot); free(t->saved_snapshot);
    t->name = NULL; t->snapshot = NULL; t->saved_snapshot = NULL;
}

// Save current editor state into a tab structure (caller must manage append)
static struct Tab make_tab_from_editor(void) {
    struct Tab t = {0};
    t.name = E.filename ? strdup(E.filename) : strdup("");
    t.snapshot = serialize_buffer();
    t.saved_snapshot = E.saved_snapshot ? strdup(E.saved_snapshot) : NULL;
    t.cx = E.cx; t.cy = E.cy; t.row_offset = E.row_offset; t.col_offset = E.col_offset;
    return t;
}

static int append_tab(struct Tab t) {
    struct Tab *newtabs = realloc(Tabs, sizeof(struct Tab) * (NumTabs + 1));
    if (!newtabs) { free_tab(&t); return -1; }
    Tabs = newtabs; Tabs[NumTabs] = t; NumTabs++; return NumTabs - 1;
}

// Save current editor into the tabs list as a new tab and return its index
static int push_current_to_tab_list(void) {
    struct Tab t = make_tab_from_editor();
    return append_tab(t);
}

// Insert a tab at position `pos` shifting existing tabs to the right.
// Returns the inserted index or -1 on failure.
static int insert_tab_at(int pos, struct Tab t) {
    if (pos < 0) pos = 0;
    if (pos > NumTabs) pos = NumTabs;
    struct Tab *newtabs = realloc(Tabs, sizeof(struct Tab) * (NumTabs + 1));
    if (!newtabs) { free_tab(&t); return -1; }
    Tabs = newtabs;
    // shift right
    for (int i = NumTabs; i > pos; i--) Tabs[i] = Tabs[i-1];
    Tabs[pos] = t;
    NumTabs++;
    if (CurTab >= pos) CurTab++; // adjust current tab index if it shifted
    return pos;
}

// Push the "home" snapshot (the current home buffer) to the front of the tabs list.
// If we are currently on a stored tab, copy HomeTab; otherwise snapshot current editor.
static int push_home_to_front(void) {
    struct Tab t = {0};
    if (CurTab == -1) {
        t = make_tab_from_editor();
    } else {
        if (HaveHomeTab) {
            t.name = HomeTab.name ? strdup(HomeTab.name) : strdup("");
            t.snapshot = HomeTab.snapshot ? strdup(HomeTab.snapshot) : NULL;
            t.saved_snapshot = HomeTab.saved_snapshot ? strdup(HomeTab.saved_snapshot) : NULL;
            t.cx = HomeTab.cx; t.cy = HomeTab.cy; t.row_offset = HomeTab.row_offset; t.col_offset = HomeTab.col_offset;
        } else {
            t = make_tab_from_editor();
        }
    }
    return insert_tab_at(0, t);
} 

// Load tab at index into the editor (saving current into its slot first)
static int switch_to_tab(int idx) {
    if (idx < 0 || idx >= NumTabs) return -1;
    // save current into current slot if CurTab >=0
    if (CurTab >= 0 && CurTab < NumTabs) {
        free(Tabs[CurTab].snapshot);
        Tabs[CurTab].snapshot = serialize_buffer();
        free(Tabs[CurTab].name); Tabs[CurTab].name = E.filename ? strdup(E.filename) : strdup("");
        free(Tabs[CurTab].saved_snapshot);
        Tabs[CurTab].saved_snapshot = E.saved_snapshot ? strdup(E.saved_snapshot) : NULL;
        Tabs[CurTab].cx = E.cx; Tabs[CurTab].cy = E.cy; Tabs[CurTab].row_offset = E.row_offset; Tabs[CurTab].col_offset = E.col_offset;
    } else if (CurTab == -1) {
        // when leaving the home buffer, capture its state so "tab 1" can restore it
        free_tab(&HomeTab);
        HomeTab = make_tab_from_editor();
        HaveHomeTab = 1;
    }
    // load target
    free(E.filename); E.filename = NULL;
    editorFreeRows();
    if (Tabs[idx].snapshot) load_buffer_from_string(Tabs[idx].snapshot);
    if (Tabs[idx].name && Tabs[idx].name[0]) E.filename = strdup(Tabs[idx].name);
    free(E.saved_snapshot);
    E.saved_snapshot = Tabs[idx].saved_snapshot ? strdup(Tabs[idx].saved_snapshot) : NULL;
    E.cx = Tabs[idx].cx; E.cy = Tabs[idx].cy; E.row_offset = Tabs[idx].row_offset; E.col_offset = Tabs[idx].col_offset;
    CurTab = idx;
    editorScroll(); return 0;
}

// forward-declare small helpers used by close_tab
static void editorAppendRow(const char *s);

// Tab management helpers
static void save_current_to_home(void) {
    free_tab(&HomeTab);
    HomeTab = make_tab_from_editor();
    HaveHomeTab = 1;
}

static void load_tab_snapshot(int idx) {
    free(E.filename); E.filename = NULL;
    editorFreeRows();
    if (Tabs[idx].snapshot) load_buffer_from_string(Tabs[idx].snapshot);
    if (Tabs[idx].name && Tabs[idx].name[0]) E.filename = strdup(Tabs[idx].name);
    free(E.saved_snapshot);
    E.saved_snapshot = Tabs[idx].saved_snapshot ? strdup(Tabs[idx].saved_snapshot) : NULL;
    E.cx = Tabs[idx].cx; E.cy = Tabs[idx].cy; E.row_offset = Tabs[idx].row_offset; E.col_offset = Tabs[idx].col_offset;
    CurTab = idx;
    editorScroll();
}

static void editorSeekLine(int lineno) {
    if (lineno < 1) lineno = 1; if (lineno > E.numrows) lineno = E.numrows;
    E.cy = lineno - 1; if (E.cy < 0) E.cy = 0; E.cx = 0; editorScroll();
}
static void save_snapshot(void);
static int switch_to_home(void);

// Close tab at index; if closing the currently-visible tab, load a neighboring tab
static int close_tab(int idx) {
    if (idx < 0 || idx >= NumTabs) return -1;
    int was_current = (CurTab == idx);
    // free the tab resources
    free_tab(&Tabs[idx]);
    // compact array
    for (int j = idx; j < NumTabs - 1; j++) Tabs[j] = Tabs[j+1];
    NumTabs--;
    if (NumTabs == 0) {
        free(Tabs); Tabs = NULL; CurTab = -1;
        if (was_current) {
            if (HaveHomeTab) {
                switch_to_home();
            } else {
                free(E.filename); E.filename = NULL;
                editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
            }
        }
        return 0;
    }
    if (was_current) {
        int newidx = idx; if (newidx >= NumTabs) newidx = NumTabs - 1;
        switch_to_tab(newidx);
    } else {
        if (CurTab > idx) CurTab--; // shifted left
    }
    return 0;
}

// Switch to the saved "home" (current buffer) state; if none captured, load empty buffer
static int switch_to_home(void) {
    // save current visible tab into its slot if necessary
    if (CurTab >= 0 && CurTab < NumTabs) {
        free(Tabs[CurTab].snapshot);
        Tabs[CurTab].snapshot = serialize_buffer();
        free(Tabs[CurTab].name); Tabs[CurTab].name = E.filename ? strdup(E.filename) : strdup("");
        free(Tabs[CurTab].saved_snapshot);
        Tabs[CurTab].saved_snapshot = E.saved_snapshot ? strdup(E.saved_snapshot) : NULL;
        Tabs[CurTab].cx = E.cx; Tabs[CurTab].cy = E.cy; Tabs[CurTab].row_offset = E.row_offset; Tabs[CurTab].col_offset = E.col_offset;
    }
    // load home snapshot
    free(E.filename); E.filename = NULL;
    editorFreeRows();
    if (HaveHomeTab && HomeTab.snapshot) {
        load_buffer_from_string(HomeTab.snapshot);
        if (HomeTab.name && HomeTab.name[0]) E.filename = strdup(HomeTab.name);
        free(E.saved_snapshot);
        E.saved_snapshot = HomeTab.saved_snapshot ? strdup(HomeTab.saved_snapshot) : NULL;
        E.cx = HomeTab.cx; E.cy = HomeTab.cy; E.row_offset = HomeTab.row_offset; E.col_offset = HomeTab.col_offset;
    } else {
        // no saved home state: show welcome page (keep a single empty buffer underneath)
        editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
        E.welcome_visible = 1;
        E.help_from_welcome = 0;
        E.browser_from_welcome = 0;
    }
    CurTab = -1; editorScroll(); return 0;
}


static void die(const char *s) {
    perror(s);
    exit(1);
}

// Forward declarations used by embedded terminal
static void editorRefreshScreen(void);
static void show_file_browser(void);
static int alphasort_entries(const void *a, const void *b);
static void open_file_in_new_tab(const char *fname);
static void disableRawMode(void);
static void enableRawMode(void);
static int switch_to_home(void);

// Forward declarations for functions referenced by tab code
static void editorFreeRows(void);
static char *serialize_buffer(void);
static void load_buffer_from_string(const char *s);
static void editorScroll(void);

static void set_language_from_filename(const char *fname) {
    const char *lang = NULL;
    const char *ext = strrchr(fname, '.');
    if (ext) {
        if (strcasecmp(ext, ".c") == 0 || strcasecmp(ext, ".h") == 0) lang = "c";
        else if (strcasecmp(ext, ".cpp") == 0 || strcasecmp(ext, ".cc") == 0 || strcasecmp(ext, ".cxx") == 0 || strcasecmp(ext, ".hpp") == 0) lang = "cpp";
        else if (strcasecmp(ext, ".py") == 0) lang = "python";
        else if (strcasecmp(ext, ".js") == 0) lang = "javascript";
        else if (strcasecmp(ext, ".jsx") == 0) lang = "javascriptreact";
        else if (strcasecmp(ext, ".ts") == 0) lang = "typescript";
        else if (strcasecmp(ext, ".tsx") == 0) lang = "typescriptreact";
        else if (strcasecmp(ext, ".java") == 0) lang = "java";
        else if (strcasecmp(ext, ".go") == 0) lang = "go";
        else if (strcasecmp(ext, ".rs") == 0) lang = "rust";
        else if (strcasecmp(ext, ".rb") == 0) lang = "ruby";
        else if (strcasecmp(ext, ".php") == 0) lang = "php";
        else if (strcasecmp(ext, ".html") == 0 || strcasecmp(ext, ".htm") == 0) lang = "html";
        else if (strcasecmp(ext, ".css") == 0) lang = "css";
        else if (strcasecmp(ext, ".json") == 0) lang = "json";
        else if (strcasecmp(ext, ".xml") == 0) lang = "xml";
        else if (strcasecmp(ext, ".yml") == 0 || strcasecmp(ext, ".yaml") == 0) lang = "yaml";
        else if (strcasecmp(ext, ".toml") == 0) lang = "toml";
        else if (strcasecmp(ext, ".md") == 0) lang = "markdown";
        else if (strcasecmp(ext, ".sh") == 0 || strcasecmp(ext, ".bash") == 0) lang = "bash";
        else if (strcasecmp(ext, ".zsh") == 0) lang = "zsh";
        else if (strcasecmp(ext, ".ps1") == 0) lang = "ps1";
        else if (strcasecmp(ext, ".swift") == 0) lang = "swift";
        else if (strcasecmp(ext, ".kt") == 0) lang = "kotlin";
        else if (strcasecmp(ext, ".cs") == 0) lang = "cs";
        else if (strcasecmp(ext, ".lua") == 0) lang = "lua";
        else if (strcasecmp(ext, ".r") == 0) lang = "r";
        else if (strcasecmp(ext, ".sql") == 0) lang = "sql";
        else if (strcasecmp(ext, ".pl") == 0 || strcasecmp(ext, ".pm") == 0) lang = "perl";
        else if (strcasecmp(ext, ".ps") == 0) lang = NULL;
    }
    if (lang) set_language(lang);
}

// Run an embedded shell using forkpty; temporarily restore terminal mode so shell behaves normally
static void run_embedded_terminal(void) {
    const char *shell = getenv("SHELL"); if (!shell) shell = "/bin/sh";

    // ensure terminal is in raw mode so we can forward keystrokes to the PTY master
    enableRawMode();

    int masterfd; pid_t pid = forkpty(&masterfd, NULL, NULL, NULL);
    if (pid == -1) {
        snprintf(E.msg, sizeof(E.msg), "forkpty failed: %s", strerror(errno)); E.msg_time = time(NULL);
        enableRawMode();
        return;
    }
    if (pid == 0) {
        // child: exec shell (this replaces the child)
        execlp(shell, shell, (char *)NULL);
        _exit(127);
    }

    // parent: mirror window size to pty
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) ioctl(masterfd, TIOCSWINSZ, &ws);

    // we'll forward control bytes (Ctrl-C, Ctrl-Z, Ctrl-]) to the child directly

    // forwarding loop: stdin -> masterfd, masterfd -> stdout
    int flags_master = fcntl(masterfd, F_GETFL, 0);
    fcntl(masterfd, F_SETFL, flags_master | O_NONBLOCK);
    int flags_stdin = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags_stdin & ~O_NONBLOCK);

    char ibuf[4096], obuf[4096];
    int child_exited = 0;
    while (!child_exited) {
        fd_set rfds;
        FD_ZERO(&rfds);
        if (winch_received) {
            struct winsize _ws;
            if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &_ws) == 0) ioctl(masterfd, TIOCSWINSZ, &_ws);
            getWindowSize();
            editorScroll();
            winch_received = 0;
        }
        FD_SET(masterfd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (masterfd > STDIN_FILENO) ? masterfd : STDIN_FILENO;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rv < 0) {
            if (errno == EINTR) {
                // check child status
                int st;
                pid_t w = waitpid(pid, &st, WNOHANG);
                if (w == pid) { child_exited = 1; break; }
                continue;
            }
            break;
        }
        if (FD_ISSET(masterfd, &rfds)) {
            ssize_t r = read(masterfd, obuf, sizeof(obuf));
            if (r > 0) write(STDOUT_FILENO, obuf, (size_t)r);
            else if (r == 0) { // EOF from child pty
                int st; waitpid(pid, &st, 0); child_exited = 1; break;
            }
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t r = read(STDIN_FILENO, ibuf, sizeof(ibuf));
            if (r > 0) {
                // handle special control bytes individually, forward others
                for (ssize_t bi = 0; bi < r; bi++) {
                    unsigned char b = (unsigned char)ibuf[bi];
                    if (b == 29) { // Ctrl-] escape: kill child and return
                        kill(pid, SIGTERM);
                        int st; waitpid(pid, &st, 0); child_exited = 1; break;
                    } else if (b == 3) { // Ctrl-C
                        kill(pid, SIGINT);
                    } else if (b == 26) { // Ctrl-Z
                        kill(pid, SIGTSTP);
                    } else {
                        ssize_t w = write(masterfd, &b, 1);
                        (void)w;
                    }
                }
                if (child_exited) break;
            } else if (r == 0) {
                // EOF on real stdin (Ctrl-D at start): close master to send EOF to child
                close(masterfd);
                int st; waitpid(pid, &st, 0); child_exited = 1; break;
            }
        }
        // check if child exited without us noticing
        int st; pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) { child_exited = 1; break; }
    }

    // ensure child is reaped
    waitpid(pid, NULL, 0);

    // restore editor raw mode and redraw
    enableRawMode();
    editorRefreshScreen();
    snprintf(E.msg, sizeof(E.msg), "Returned from embedded terminal"); E.msg_time = time(NULL);
}

// Run a command in a PTY, capturing output to logfile and pane, allowing interactive input
static int run_command_in_pty(const char *shell_cmd, FILE *out) {
    // temporarily disable raw mode to allow normal terminal behavior
    disableRawMode();



    // clear the screen before running the command to avoid leftover text
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    int masterfd;
    pid_t pid = forkpty(&masterfd, NULL, NULL, NULL);
    if (pid == -1) {
        enableRawMode();
        return -1;
    }
    if (pid == 0) {
        // child: execute the command
        execlp("/bin/sh", "sh", "-c", shell_cmd, (char *)NULL);
        _exit(127);
    }

    // parent: mirror window size to pty
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) ioctl(masterfd, TIOCSWINSZ, &ws);

    // set masterfd non-blocking
    int flags_master = fcntl(masterfd, F_GETFL, 0);
    fcntl(masterfd, F_SETFL, flags_master | O_NONBLOCK);
    // set stdin blocking
    int flags_stdin = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags_stdin & ~O_NONBLOCK);

    char buf[1024];
    char output_buf[4096];
    int buf_pos = 0;
    int child_exited = 0;
    int exitcode = 0;

    while (!child_exited) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(masterfd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        int maxfd = (masterfd > STDIN_FILENO) ? masterfd : STDIN_FILENO;
        int rv = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (rv < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (FD_ISSET(masterfd, &rfds)) {
            ssize_t r = read(masterfd, buf, sizeof(buf));
            if (r > 0) {
                // write to terminal
                write(STDOUT_FILENO, buf, (size_t)r);
                // write to log
                fwrite(buf, 1, (size_t)r, out);
                fflush(out);
                // buffer for pane
                for (ssize_t i = 0; i < r; i++) {
                    if (buf[i] == '\n' || buf_pos >= (int)sizeof(output_buf) - 1) {
                        output_buf[buf_pos] = '\0';
                        output_append_colored_line(output_buf);
                        buf_pos = 0;
                    } else {
                        output_buf[buf_pos++] = buf[i];
                    }
                }
            } else if (r == 0) {
                child_exited = 1;
            }
        }
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            ssize_t r = read(STDIN_FILENO, buf, sizeof(buf));
            if (r > 0) {
                write(masterfd, buf, (size_t)r);
            } else if (r == 0) {
                close(masterfd);
                child_exited = 1;
            }
        }
        // check if child exited
        int st;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            exitcode = WEXITSTATUS(st);
            child_exited = 1;
        }
    }

    // flush any remaining output
    if (buf_pos > 0) {
        output_buf[buf_pos] = '\0';
        output_append_colored_line(output_buf);
    }

    // ensure child is reaped
    waitpid(pid, NULL, 0);

    // restore editor raw mode and redraw
    enableRawMode();
    editorRefreshScreen();

    return exitcode;
}

static int __raw_inited = 0;

static void disableRawMode(void) {
    /* restore primary screen buffer so previous terminal scrollback isn't visible */
    write(STDOUT_FILENO, "\x1b[?1049l", 8);
    if (__raw_inited) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
        __raw_inited = 0;
    }
}

/*
 * handle_terminate_signal
 * -----------------------
 * Purpose: Ensure the editor restores the terminal to a sane state when
 * the process is terminated by signals (SIGINT, SIGTERM, SIGHUP, SIGQUIT).
 * Rationale: Without restoring the terminal (disabling raw mode and
 * switching back from the alternate screen buffer), the user's shell can
 * be left unable to echo or display text properly after the editor exits.
 *
 * IMPORTANT: Do not remove or change this handler unless you understand
 * the terminal state machine and have a clear reason (and tests) proving
 * that the new approach restores the terminal reliably in all expected
 * termination scenarios (including signals and abrupt exits).
 */
static void handle_terminate_signal(int sig) {
    disableRawMode();
    /* restore default handler and re-raise so process terminates as usual */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
    raise(sig);
}

/* SIGWINCH handler: mark that the window has been resized */
static void handle_sigwinch(int sig) { (void)sig; winch_received = 1; }

static void enableRawMode(void) {
    if (!__raw_inited) {
        if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
        atexit(disableRawMode);
        __raw_inited = 1;
    }

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(ICRNL | IXON | ISTRIP | INPCK | BRKINT);
    raw.c_oflag &= ~(OPOST);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cflag |= (CS8);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
    // set cursor to blinking bar style
    write(STDOUT_FILENO, "\x1b[5 q", 5);
    /* switch to alternate screen buffer so editor session doesn't pollute scrollback */
    write(STDOUT_FILENO, "\x1b[?1049h", 8);
}

static void getWindowSize(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        E.screenrows = 24;
        E.screencols = 80;
    } else {
        E.screenrows = ws.ws_row;
        E.screencols = ws.ws_col;
    }
}

static void editorAppendRow(const char *s) {
    char *line = strdup(s ? s : "");
    if (!line) die("strdup");
    char **newrows = realloc(E.rows, sizeof(char*) * (E.numrows + 1));
    if (!newrows) die("realloc");
    E.rows = newrows;
    E.rows[E.numrows] = line;
    E.numrows++;
}

static void editorInsertRow(int at, const char *s) {
    if (at < 0 || at > E.numrows) return; // guard
    char *line = strdup(s ? s : "");
    if (!line) die("strdup");
    char **newrows = realloc(E.rows, sizeof(char*) * (E.numrows + 1));
    if (!newrows) die("realloc");
    E.rows = newrows;
    memmove(&E.rows[at+1], &E.rows[at], sizeof(char*) * (E.numrows - at));
    E.rows[at] = line;
    E.numrows++;
}

static void editorFreeRows(void) {
    for (int i = 0; i < E.numrows; i++) free(E.rows[i]);
    free(E.rows);
    E.rows = NULL;
    E.numrows = 0;
}

// Serialize buffer to a single string (caller must free)
static char *serialize_buffer(void) {
    size_t total = 0;
    for (int i = 0; i < E.numrows; i++) total += strlen(E.rows[i]) + 1; // +1 for newline
    char *s = malloc(total + 1);
    if (!s) return NULL;
    char *p = s;
    for (int i = 0; i < E.numrows; i++) {
        size_t len = strlen(E.rows[i]);
        memcpy(p, E.rows[i], len);
        p += len;
        *p++ = '\n';
    }
    if (p != s) p[-1] = '\0'; else *p = '\0';
    return s;
}

// Check if current buffer has unsaved changes
static int is_unsaved(void) {
    if (!E.saved_snapshot) return 1; // no saved snapshot means unsaved
    char *current = serialize_buffer();
    if (!current) return 1; // error, assume unsaved
    int unsaved = strcmp(current, E.saved_snapshot) != 0;
    free(current);
    return unsaved;
}

// Simple runtime config (parsed from ~/.jedrc or $XDG_CONFIG_HOME/jed/config)
struct JedConfig {
    int indent_size;
    int use_tabs; // 0 = false, 1 = true
    int show_line_numbers;
    int auto_save_interval; // seconds, 0 = disabled
    int show_tab_bar; // 0 = hide tab bar, 1 = show tab bar
} conf = { .indent_size = 4, .use_tabs = 0, .show_line_numbers = 1, .auto_save_interval = 0, .show_tab_bar = 1 };

static void load_config(void) {
    const char *xdg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (xdg && *xdg) snprintf(path, sizeof(path), "%s/jed/config", xdg);
    else {
        const char *home = getenv("HOME");
        if (!home) return;
        snprintf(path, sizeof(path), "%s/.jedrc", home);
    }
    FILE *f = fopen(path, "r");
    if (!f) return; // no config is fine
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // strip leading spaces
        char *p = line; while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '\0' || *p == '#' || *p == ';') continue; // comment or empty
        // find '='
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = p; char *val = eq + 1;
        // trim trailing spaces from key
        char *kend = key + strlen(key) - 1; while (kend > key && isspace((unsigned char)*kend)) *kend-- = '\0';
        // trim leading spaces from val
        while (*val && isspace((unsigned char)*val)) val++;
        // trim trailing newline/spaces
        char *vend = val + strlen(val) - 1; while (vend > val && isspace((unsigned char)*vend)) *vend-- = '\0';
        if (strcasecmp(key, "indent_size") == 0) conf.indent_size = atoi(val);
        else if (strcasecmp(key, "use_tabs") == 0) conf.use_tabs = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcasecmp(key, "show_line_numbers") == 0) conf.show_line_numbers = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
        else if (strcasecmp(key, "auto_save_interval") == 0) conf.auto_save_interval = atoi(val);
        else if (strcasecmp(key, "show_tab_bar") == 0) conf.show_tab_bar = (strcasecmp(val, "true") == 0 || strcmp(val, "1") == 0);
    }
    fclose(f);
}


// Compute leading indentation in spaces (tabs count as conf.indent_size)
static int compute_leading_spaces(const char *s) {
    int i = 0; int width = 0;
    while (s[i]) {
        if (s[i] == ' ') { width += 1; i++; }
        else if (s[i] == '\t') { width += conf.indent_size; i++; }
        else break;
    }
    return width;
}

// Create indentation string for a given space width, respecting conf.use_tabs
static char *make_indent_string(int indent_spaces) {
    if (indent_spaces <= 0) return strdup("");
    if (conf.use_tabs) {
        int tabs = indent_spaces / conf.indent_size;
        int spaces = indent_spaces % conf.indent_size;
        int need = tabs + spaces;
        char *s = malloc(need + 1);
        if (!s) return NULL;
        int p = 0;
        for (int i = 0; i < tabs; i++) s[p++] = '\t';
        for (int i = 0; i < spaces; i++) s[p++] = ' ';
        s[p] = '\0';
        return s;
    } else {
        char *s = malloc(indent_spaces + 1);
        if (!s) return NULL;
        for (int i = 0; i < indent_spaces; i++) s[i] = ' ';
        s[indent_spaces] = '\0';
        return s;
    }
}

static void load_buffer_from_string(const char *s) {
    editorFreeRows();
    if (!s || *s == '\0') {
        editorAppendRow("");
        return;
    }
    const char *p = s;
    const char *line_start = p;
    while (*p) {
        if (*p == '\n') {
            size_t len = p - line_start;
            char *row = malloc(len + 1);
            if (!row) die("malloc");
            memcpy(row, line_start, len);
            row[len] = '\0';
            char **newrows = realloc(E.rows, sizeof(char*) * (E.numrows + 1));
            if (!newrows) die("realloc");
            E.rows = newrows;
            E.rows[E.numrows++] = row;
            p++;
            line_start = p;
        } else {
            p++;
        }
    }
    // last line if not terminated by newline
    if (p > line_start) {
        size_t len = p - line_start;
        char *row = malloc(len + 1);
        if (!row) die("malloc");
        memcpy(row, line_start, len);
        row[len] = '\0';
        char **newrows = realloc(E.rows, sizeof(char*) * (E.numrows + 1));
        if (!newrows) die("realloc");
        E.rows = newrows;
        E.rows[E.numrows++] = row;
    }
}

// History
static int readKey(void);
static void editorRefreshScreen(void);

static void save_snapshot(void) {
    char *snapshot = serialize_buffer();
    if (!snapshot) return;
    // avoid pushing duplicate consecutive snapshots
    if (E.history_count > 0 && E.history[E.history_count - 1] && strcmp(E.history[E.history_count - 1], snapshot) == 0) {
        free(snapshot);
        return;
    }
    // truncate future redo history
    for (int i = E.history_idx + 1; i < E.history_count; i++) {
        free(E.history[i]);
    }
    E.history_count = E.history_idx + 1;
    char **newhist = realloc(E.history, sizeof(char*) * (E.history_count + 1));
    if (!newhist) { free(snapshot); return; }
    E.history = newhist;
    E.history[E.history_count++] = snapshot;
    E.history_idx = E.history_count - 1;
    // cap history
    if (E.history_count > HISTORY_LIMIT) {
        // drop oldest
        free(E.history[0]);
        memmove(&E.history[0], &E.history[1], sizeof(char*) * (E.history_count - 1));
        E.history_count--;
        E.history_idx--;
        char **shrink = realloc(E.history, sizeof(char*) * E.history_count);
        if (shrink) E.history = shrink;
    }
}

static long long now_ms(void) {
    struct timeval tv; gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000LL + tv.tv_usec / 1000;
}

static void begin_batch(void) { E.batching++; }
static void end_batch(void) { if (E.batching > 0) E.batching--; }

static void maybe_snapshot(int kind) {
    if (E.batching) return;
    long long now = now_ms();
    if (E.last_edit_kind == kind && (now - E.last_edit_time_ms) < EDIT_GROUP_MS) {
        E.last_edit_time_ms = now;
        return;
    }
    save_snapshot();
    // also write swap for crash recovery if we have a filename
    if (E.filename) {
        // write swap file using current buffer
        char *s = serialize_buffer();
        if (s) {
            char *sw = NULL;
            // build swap path
            char *base = strrchr(E.filename, '/');
            const char *b = base ? base + 1 : E.filename;
            if (base) {
                size_t dlen = base - E.filename;
                size_t need = dlen + 1 + 1 + strlen(b) + 5 + 1; // dir + '/' + '.' + base + '.swp' + NUL
                sw = malloc(need);
                if (sw) snprintf(sw, need, "%.*s/.%s.swp", (int)dlen, E.filename, b);
            } else {
                size_t need = 1 + strlen(b) + 4 + 1; // '.' + base + '.swp' + NUL
                sw = malloc(need);
                if (sw) snprintf(sw, need, ".%s.swp", b);
            }
            if (sw) {
                FILE *f = fopen(sw, "w");
                if (f) {
                    fwrite(s, 1, strlen(s), f);
                    fclose(f);
                    // attempt to set restrictive perms
                    chmod(sw, S_IRUSR | S_IWUSR);
                }
                free(sw);
            }
            free(s);
        }
    }
    E.last_edit_kind = kind;
    E.last_edit_time_ms = now;
}


static void load_snapshot(int idx) {
    if (idx < 0 || idx >= E.history_count) return;
    load_buffer_from_string(E.history[idx]);
    if (E.cy >= E.numrows) E.cy = E.numrows - 1;
    if (E.cy < 0) E.cy = 0;
    if (E.cx > (int)strlen(E.rows[E.cy])) E.cx = strlen(E.rows[E.cy]);
}

static void undo_cmd(void) {
    if (E.history_idx > 0) {
        E.history_idx--;
        load_snapshot(E.history_idx);
        // set message
        snprintf(E.msg, sizeof(E.msg), "Undo (%d/%d)", E.history_idx, E.history_count - 1); E.msg_time = time(NULL);
    } else {
        snprintf(E.msg, sizeof(E.msg), "No more undo (%d/%d)", E.history_idx, E.history_count - 1); E.msg_time = time(NULL);
    }
}

static void redo_cmd(void) {
    if (E.history_idx + 1 < E.history_count) {
        E.history_idx++;
        load_snapshot(E.history_idx);
        snprintf(E.msg, sizeof(E.msg), "Redo (%d/%d)", E.history_idx, E.history_count - 1); E.msg_time = time(NULL);
    } else {
        snprintf(E.msg, sizeof(E.msg), "No more redo (%d/%d)", E.history_idx, E.history_count - 1); E.msg_time = time(NULL);
    }
}

/* forward declarations for functions used by the executor */
static int execute_command(const char *cmd);
static void editorInsertChar(int c);
static void editorInsertNewline(void);
static void editorScroll(void);
static void paste_at_cursor(void);

// helper: delete line at index (0-based)
static void delete_line_at(int idx) {
    if (idx < 0 || idx >= E.numrows) return;
    free(E.rows[idx]);
    memmove(&E.rows[idx], &E.rows[idx+1], sizeof(char*) * (E.numrows - idx - 1));
    E.numrows--;
    if (E.numrows == 0) editorAppendRow("");
    if (E.cy >= E.numrows) E.cy = E.numrows - 1;
    if (E.cy < 0) E.cy = 0;
}


static void copy_line_n(int n) {
    if (n < 1 || n > E.numrows) {
        snprintf(E.msg, sizeof(E.msg), "Bad line %d", n); E.msg_time = time(NULL); return;
    }
    free(E.clipboard);
    E.clipboard = strdup(E.rows[n - 1]);
    snprintf(E.msg, sizeof(E.msg), "Copied line %d", n); E.msg_time = time(NULL);
}

static void copy_lines_list(int *list, int cnt) {
    if (cnt <= 0) return;
    // calculate total length
    size_t total_len = 0;
    for (int k = 0; k < cnt; k++) {
        int ln = list[k];
        if (ln < 1 || ln > E.numrows) continue;
        total_len += strlen(E.rows[ln - 1]) + 1; // +1 for \n
    }
    if (total_len == 0) {
        snprintf(E.msg, sizeof(E.msg), "No valid lines to copy"); E.msg_time = time(NULL); return;
    }
    char *buf = malloc(total_len);
    if (!buf) { snprintf(E.msg, sizeof(E.msg), "Memory error"); E.msg_time = time(NULL); return; }
    buf[0] = '\0';
    for (int k = 0; k < cnt; k++) {
        int ln = list[k];
        if (ln < 1 || ln > E.numrows) continue;
        if (buf[0]) strcat(buf, "\n");
        strcat(buf, E.rows[ln - 1]);
    }
    free(E.clipboard);
    E.clipboard = buf;
    snprintf(E.msg, sizeof(E.msg), "Copied %d lines", cnt); E.msg_time = time(NULL);
}

static void delete_last_words(int n) {
    if (n <= 0) return;
    char *row = E.rows[E.cy];
    int len = (int)strlen(row);
    if (len == 0) return;
    // find the last n words
    int word_count = 0;
    int pos = len - 1;
    // skip trailing whitespace
    while (pos >= 0 && (row[pos] == ' ' || row[pos] == '\t')) pos--;
    while (pos >= 0 && word_count < n) {
        // skip non-whitespace (word)
        while (pos >= 0 && row[pos] != ' ' && row[pos] != '\t') pos--;
        word_count++;
        // skip whitespace
        while (pos >= 0 && (row[pos] == ' ' || row[pos] == '\t')) pos--;
    }
    if (word_count < n) pos = -1; // delete all if not enough words
    int new_len = pos + 1;
    if (new_len < 0) new_len = 0;
    row[new_len] = '\0';
    E.cx = new_len;
    if (E.cx > len) E.cx = len; // shouldn't happen
    editorScroll();
}

static void delete_first_words(int n) {
    if (n <= 0) return;
    char *row = E.rows[E.cy];
    int len = (int)strlen(row);
    if (len == 0) return;
    // find the first n words
    int word_count = 0;
    int pos = 0;
    // skip leading whitespace
    while (pos < len && (row[pos] == ' ' || row[pos] == '\t')) pos++;
    while (pos < len && word_count < n) {
        // skip word
        while (pos < len && row[pos] != ' ' && row[pos] != '\t') pos++;
        word_count++;
        // skip whitespace
        while (pos < len && (row[pos] == ' ' || row[pos] == '\t')) pos++;
    }
    if (word_count < n) pos = len; // delete all
    // shift the rest
    memmove(row, row + pos, len - pos + 1);
    E.cx = 0;
    editorScroll();
}

static void paste_at_line_n(int n) {
    if (!E.clipboard) { snprintf(E.msg, sizeof(E.msg), "Clipboard empty"); E.msg_time = time(NULL); return; }
    int at = n;
    if (at < 1) at = 1;
    if (at > E.numrows) at = E.numrows + 1;
    int target_idx = at - 1;
    const char *p = E.clipboard;
    // Helper: iterate over clipboard lines, splitting on '\n'
    // If target exists and is non-blank, append first clipboard line to it and insert remaining lines after.
    if (target_idx >= 0 && target_idx < E.numrows) {
        char *t = E.rows[target_idx];
        int j = 0; int onlyws = 1;
        while (t[j]) { if (t[j] != ' ' && t[j] != '\t' && t[j] != '\r' && t[j] != '\n') { onlyws = 0; break; } j++; }
        // find first clipboard line
        const char *nl = strchr(p, '\n');
        size_t first_len = nl ? (size_t)(nl - p) : strlen(p);
        // determine insertion column: always append for pl<N>
        int insert_col = (int)strlen(E.rows[target_idx]);
        if (onlyws) {
            // replace existing blank row with first line
            free(E.rows[target_idx]);
            E.rows[target_idx] = malloc(first_len + 1);
            if (!E.rows[target_idx]) die("malloc");
            memcpy(E.rows[target_idx], p, first_len);
            E.rows[target_idx][first_len] = '\0';
            E.cy = target_idx;
            E.cx = (int)strlen(E.rows[target_idx]);
            editorScroll();
            // insert remaining lines after target
            const char *rest = nl ? (nl + 1) : NULL;
            while (rest && *rest) {
                const char *n2 = strchr(rest, '\n');
                size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
                begin_batch();
                editorInsertRow(target_idx + 1, "");
                // replace the newly inserted row with proper content
                free(E.rows[target_idx + 1]);
                E.rows[target_idx + 1] = malloc(L + 1);
                if (!E.rows[target_idx + 1]) die("malloc");
                memcpy(E.rows[target_idx + 1], rest, L); E.rows[target_idx + 1][L] = '\0';
                end_batch();
                target_idx++;
                rest = n2 ? (n2 + 1) : NULL;
            }
            snprintf(E.msg, sizeof(E.msg), "Pasted into line %d", at); E.msg_time = time(NULL);
            /* snapshot after paste */
            save_snapshot();
            return;
        } else {
            // insert first line at insert_col (not necessarily append)
            int oldlen = (int)strlen(E.rows[target_idx]);
            int ins = insert_col;
            if (ins < 0) ins = 0; if (ins > oldlen) ins = oldlen;
            char *newrow = malloc(oldlen + (int)first_len + 1);
            if (!newrow) die("malloc");
            // copy prefix
            memcpy(newrow, E.rows[target_idx], ins);
            // copy clipboard first fragment
            memcpy(newrow + ins, p, first_len);
            // copy suffix
            memcpy(newrow + ins + first_len, E.rows[target_idx] + ins, oldlen - ins + 1);
            free(E.rows[target_idx]);
            E.rows[target_idx] = newrow;
            // if there are more lines in clipboard, insert them after target
            const char *rest = nl ? (nl + 1) : NULL;
            int last_idx = target_idx;
            while (rest && *rest) {
                const char *n2 = strchr(rest, '\n');
                size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
                begin_batch();
                editorInsertRow(last_idx + 1, "");
                free(E.rows[last_idx + 1]);
                E.rows[last_idx + 1] = malloc(L + 1);
                if (!E.rows[last_idx + 1]) die("malloc");
                memcpy(E.rows[last_idx + 1], rest, L); E.rows[last_idx + 1][L] = '\0';
                end_batch();
                last_idx++;
                rest = n2 ? (n2 + 1) : NULL;
            }
            E.cy = last_idx;
            E.cx = (int)strlen(E.rows[last_idx]);
            editorScroll();
            snprintf(E.msg, sizeof(E.msg), "Pasted at line %d", at); E.msg_time = time(NULL);
            /* snapshot after paste */
            save_snapshot();
            return;
        }
    }
    // target does not exist (append past end) -> insert all clipboard lines starting at at-1
    int insert_idx = at - 1;
    const char *rest = p;
    int last_idx = insert_idx - 1;
    while (rest && *rest) {
        const char *n2 = strchr(rest, '\n');
        size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
        begin_batch();
        editorInsertRow(last_idx + 1, "");
        free(E.rows[last_idx + 1]);
        E.rows[last_idx + 1] = malloc(L + 1);
        if (!E.rows[last_idx + 1]) die("malloc");
        memcpy(E.rows[last_idx + 1], rest, L); E.rows[last_idx + 1][L] = '\0';
        end_batch();
        last_idx++;
        rest = n2 ? (n2 + 1) : NULL;
    }
    E.cy = last_idx;
    E.cx = (int)strlen(E.rows[last_idx]);
    editorScroll();
    snprintf(E.msg, sizeof(E.msg), "Pasted at line %d", at); E.msg_time = time(NULL);
    /* snapshot after paste */
    save_snapshot();
}

static void paste_at_line_n_front(int n) {
    if (!E.clipboard) { snprintf(E.msg, sizeof(E.msg), "Clipboard empty"); E.msg_time = time(NULL); return; }
    int at = n;
    if (at < 1) at = 1;
    if (at > E.numrows) at = E.numrows + 1;
    int target_idx = at - 1;
    const char *p = E.clipboard;
    // Helper: iterate over clipboard lines, splitting on '\n'
    // If target exists and is non-blank, insert first clipboard line at front and insert remaining lines after.
    if (target_idx >= 0 && target_idx < E.numrows) {
        char *t = E.rows[target_idx];
        int j = 0; int onlyws = 1;
        while (t[j]) { if (t[j] != ' ' && t[j] != '\t' && t[j] != '\r' && t[j] != '\n') { onlyws = 0; break; } j++; }
        // find first clipboard line
        const char *nl = strchr(p, '\n');
        size_t first_len = nl ? (size_t)(nl - p) : strlen(p);
        // determine insertion column: always front for plf<N>
        int insert_col = 0;
        if (onlyws) {
            // replace existing blank row with first line
            free(E.rows[target_idx]);
            E.rows[target_idx] = malloc(first_len + 1);
            if (!E.rows[target_idx]) die("malloc");
            memcpy(E.rows[target_idx], p, first_len);
            E.rows[target_idx][first_len] = '\0';
            E.cy = target_idx;
            E.cx = (int)strlen(E.rows[target_idx]);
            editorScroll();
            // insert remaining lines after target
            const char *rest = nl ? (nl + 1) : NULL;
            while (rest && *rest) {
                const char *n2 = strchr(rest, '\n');
                size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
                begin_batch();
                editorInsertRow(target_idx + 1, "");
                // replace the newly inserted row with proper content
                free(E.rows[target_idx + 1]);
                E.rows[target_idx + 1] = malloc(L + 1);
                if (!E.rows[target_idx + 1]) die("malloc");
                memcpy(E.rows[target_idx + 1], rest, L); E.rows[target_idx + 1][L] = '\0';
                end_batch();
                target_idx++;
                rest = n2 ? (n2 + 1) : NULL;
            }
            snprintf(E.msg, sizeof(E.msg), "Pasted into line %d", at); E.msg_time = time(NULL);
            /* snapshot after paste */
            save_snapshot();
            return;
        } else {
            // insert first line at insert_col (front)
            int oldlen = (int)strlen(E.rows[target_idx]);
            int ins = insert_col;
            if (ins < 0) ins = 0; if (ins > oldlen) ins = oldlen;
            char *newrow = malloc(oldlen + (int)first_len + 1);
            if (!newrow) die("malloc");
            // copy prefix
            memcpy(newrow, E.rows[target_idx], ins);
            // copy clipboard first fragment
            memcpy(newrow + ins, p, first_len);
            // copy suffix
            memcpy(newrow + ins + first_len, E.rows[target_idx] + ins, oldlen - ins + 1);
            free(E.rows[target_idx]);
            E.rows[target_idx] = newrow;
            // if there are more lines in clipboard, insert them after target
            const char *rest = nl ? (nl + 1) : NULL;
            int last_idx = target_idx;
            while (rest && *rest) {
                const char *n2 = strchr(rest, '\n');
                size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
                begin_batch();
                editorInsertRow(last_idx + 1, "");
                free(E.rows[last_idx + 1]);
                E.rows[last_idx + 1] = malloc(L + 1);
                if (!E.rows[last_idx + 1]) die("malloc");
                memcpy(E.rows[last_idx + 1], rest, L); E.rows[last_idx + 1][L] = '\0';
                end_batch();
                last_idx++;
                rest = n2 ? (n2 + 1) : NULL;
            }
            E.cy = last_idx;
            E.cx = (int)strlen(E.rows[last_idx]);
            editorScroll();
            snprintf(E.msg, sizeof(E.msg), "Pasted at line %d", at); E.msg_time = time(NULL);
            /* snapshot after paste */
            save_snapshot();
            return;
        }
    }
    // target does not exist (append past end) -> insert all clipboard lines starting at at-1
    int insert_idx = at - 1;
    const char *rest = p;
    int last_idx = insert_idx - 1;
    while (rest && *rest) {
        const char *n2 = strchr(rest, '\n');
        size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
        begin_batch();
        editorInsertRow(last_idx + 1, "");
        free(E.rows[last_idx + 1]);
        E.rows[last_idx + 1] = malloc(L + 1);
        if (!E.rows[last_idx + 1]) die("malloc");
        memcpy(E.rows[last_idx + 1], rest, L); E.rows[last_idx + 1][L] = '\0';
        end_batch();
        last_idx++;
        rest = n2 ? (n2 + 1) : NULL;
    }
    E.cy = last_idx;
    E.cx = (int)strlen(E.rows[last_idx]);
    editorScroll();
    snprintf(E.msg, sizeof(E.msg), "Pasted at line %d", at); E.msg_time = time(NULL);
    /* snapshot after paste */
    save_snapshot();
}

static void paste_at_cursor(void) {
    if (!E.clipboard) { snprintf(E.msg, sizeof(E.msg), "Clipboard empty"); E.msg_time = time(NULL); return; }
    // Insert clipboard content at current cursor position within the current row.
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.numrows) {
        while (E.numrows <= E.cy) editorAppendRow("");
    }
    char *row = E.rows[E.cy];
    int rowlen = (int)strlen(row);
    if (E.cx < 0) E.cx = 0; if (E.cx > rowlen) E.cx = rowlen;
    const char *p = E.clipboard;
    const char *nl = strchr(p, '\n');
    if (!nl) {
        // single-line clipboard -> insert into current row
        size_t addlen = strlen(p);
        char *newrow = malloc(rowlen + (int)addlen + 1);
        if (!newrow) die("malloc");
        memcpy(newrow, row, E.cx);
        memcpy(newrow + E.cx, p, addlen);
        memcpy(newrow + E.cx + addlen, row + E.cx, rowlen - E.cx + 1);
        free(E.rows[E.cy]); E.rows[E.cy] = newrow;
        E.cx += (int)addlen;
        editorScroll();
        snprintf(E.msg, sizeof(E.msg), "Pasted at cursor"); E.msg_time = time(NULL);
        /* snapshot after paste */
        save_snapshot();
        return;
    }
    // multi-line clipboard: insert first part into current row at cursor, then insert remaining lines below
    size_t first_len = (size_t)(nl - p);
    char *first_part = malloc(E.cx + (int)first_len + 1);
    if (!first_part) die("malloc");
    memcpy(first_part, row, E.cx);
    memcpy(first_part + E.cx, p, first_len);
    first_part[E.cx + first_len] = '\0';
    free(E.rows[E.cy]); E.rows[E.cy] = first_part;
    int last_idx = E.cy;
    const char *rest = nl + 1;
    while (rest && *rest) {
        const char *n2 = strchr(rest, '\n');
        size_t L = n2 ? (size_t)(n2 - rest) : strlen(rest);
        begin_batch();
        editorInsertRow(last_idx + 1, "");
        free(E.rows[last_idx + 1]);
        E.rows[last_idx + 1] = malloc(L + 1);
        if (!E.rows[last_idx + 1]) die("malloc");
        memcpy(E.rows[last_idx + 1], rest, L); E.rows[last_idx + 1][L] = '\0';
        end_batch();
        last_idx++;
        rest = n2 ? (n2 + 1) : NULL;
    }
    E.cy = last_idx;
    E.cx = (int)strlen(E.rows[last_idx]);
    editorScroll();
    snprintf(E.msg, sizeof(E.msg), "Pasted at cursor (multi-line)"); E.msg_time = time(NULL);
    /* snapshot after paste */
    save_snapshot();
}

// parse a positive integer from s[pos], update pos via *pidx; return -1 if no digits
static int parse_number_at(const char *s, int *pidx) {
    int i = *pidx;
    int val = 0;
    int found = 0;
    while (s[i] && isdigit((unsigned char)s[i])) {
        found = 1;
        val = val * 10 + (s[i] - '0');
        i++;
    }
    if (!found) return -1;
    *pidx = i;
    return val;
}

// parse a list like "1-5,7,9" starting at s[pos]; returns a malloc'd array of ints in *out (1-based indices), count in *outc
// updates *pidx to new position; returns 0 on success, -1 on error
static int parse_line_list(const char *s, int *pidx, int **out, int *outc) {
    int i = *pidx;
    int *arr = NULL; int cnt = 0;
    while (s[i]) {
        if (!isdigit((unsigned char)s[i])) break;
        // parse first number
        int a = 0; int b = 0;
        while (s[i] && isdigit((unsigned char)s[i])) { a = a*10 + (s[i]-'0'); i++; }
        if (s[i] == '-') {
            i++;
            if (!isdigit((unsigned char)s[i])) { free(arr); return -1; }
            while (s[i] && isdigit((unsigned char)s[i])) { b = b*10 + (s[i]-'0'); i++; }
        } else {
            b = a;
        }
        // add range a..b
        if (b < a) { int tmp = a; a = b; b = tmp; }
        for (int v = a; v <= b; v++) {
            int *narr = realloc(arr, sizeof(int) * (cnt + 1));
            if (!narr) { free(arr); return -1; }
            arr = narr; arr[cnt++] = v;
        }
        if (s[i] == ',') { i++; continue; }
        break;
    }
    *pidx = i;
    *out = arr; *outc = cnt;
    return 0;
}

static int cmp_desc(const void *a, const void *b) {
    return *(int*)b - *(int*)a;
}

static void save_to_file(const char *fname) {
    const char *use = fname;
    if (!use) use = (E.filename ? E.filename : "editor_save.txt");
    FILE *f = fopen(use, "w");
    if (!f) { snprintf(E.msg, sizeof(E.msg), "Save failed: %s", strerror(errno)); E.msg_time = time(NULL); return; }
    for (int i = 0; i < E.numrows; i++) {
        fprintf(f, "%s\n", E.rows[i]);
    }
    fclose(f);
    // update saved snapshot so we can detect unsaved changes
    free(E.saved_snapshot);
    E.saved_snapshot = serialize_buffer();
    // if we're in a tab, update that tab's saved snapshot too
    if (CurTab >= 0 && CurTab < NumTabs) {
        free(Tabs[CurTab].saved_snapshot);
        Tabs[CurTab].saved_snapshot = E.saved_snapshot ? strdup(E.saved_snapshot) : NULL;
    }
    // record filename if given
    if (fname) {
        free(E.filename);
        E.filename = strdup(fname);
    }
    snprintf(E.msg, sizeof(E.msg), "Saved %s", use); E.msg_time = time(NULL);
    // remove swap file after a successful save
    if (E.filename) {
        char *base = strrchr(E.filename, '/');
        const char *b = base ? base + 1 : E.filename;
        char swapbuf[1024];
        if (base) snprintf(swapbuf, sizeof(swapbuf), "%.*s/.%s.swp", (int)(base - E.filename), E.filename, b);
        else snprintf(swapbuf, sizeof(swapbuf), ".%s.swp", b);
        unlink(swapbuf);
    }
    // if we were waiting for the user to save the build file before running
    if (E.awaiting_buildfile_save && E.post_edit_cmd[0]) {
        // run the command using internal shim so output is captured in config/last_build.log
        char runcmd[768]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", E.post_edit_cmd);
        // clear flag BEFORE running in case run triggers other saves
        E.awaiting_buildfile_save = 0; E.post_edit_cmd[0] = '\0';
        int st = execute_command(runcmd);
        (void)st; // we don't currently have a secondary run command to execute here
    }
}

static int execute_command(const char *cmd) {
    int i = 0; int L = strlen(cmd);
    // Defensive: if NumTabs > 0 but Tabs pointer is NULL, reset NumTabs to avoid dereferencing a null pointer in later loops
    if (NumTabs > 0 && !Tabs) NumTabs = 0;
    // allow quick build/run shim: "__run_shell__:<shell cmd>" will exec the shell cmd
    if (strncmp(cmd, "__run_shell__:", 14) == 0) {
        char shell_cmd[2048];
        strncpy(shell_cmd, cmd + 14, sizeof(shell_cmd) - 1);
        shell_cmd[sizeof(shell_cmd)-1] = '\0';
        // if buffer is unsaved and we have a filename, write current buffer to temp file and modify command
        if (is_unsaved() && E.filename) {
            const char *base = strrchr(E.filename, '/');
            if (base) base++; else base = E.filename;
            char temp_path[1024];
            snprintf(temp_path, sizeof(temp_path), "/tmp/editor_build_temp_%s", base);
            FILE *tf = fopen(temp_path, "w");
            if (tf) {
                for (int j = 0; j < E.numrows; j++) {
                    fprintf(tf, "%s\n", E.rows[j]);
                }
                fclose(tf);
                // replace E.filename in shell_cmd with temp_path
                char *pos = strstr(shell_cmd, E.filename);
                if (pos) {
                    size_t pre_len = pos - shell_cmd;
                    size_t fname_len = strlen(E.filename);
                    size_t post_start = pre_len + fname_len;
                    size_t temp_len = strlen(temp_path);
                    size_t new_len = pre_len + temp_len + strlen(pos + fname_len);
                    if (new_len < sizeof(shell_cmd)) {
                        memmove(pos + temp_len, pos + fname_len, strlen(pos + fname_len) + 1);
                        memcpy(pos, temp_path, temp_len);
                    }
                }
            }
        }
        const char *shell = shell_cmd;
        // ensure config directory exists (best-effort)
        mkdir("config", 0755);
        // remove any existing log file so we create a fresh file/inode for this run
        (void)unlink("config/last_build.log");
        // write output to config/last_build.log and open it
        FILE *out = fopen("config/last_build.log", "a");
        if (out) {
            // clear any previous in-editor output so the pane only reflects this run
            output_clear();
            // add a timestamp header to the log and the pane so it's clear this run's output
            time_t _t = time(NULL); struct tm *_tm = localtime(&_t); char _th[128]; if (_tm) strftime(_th, sizeof(_th), "=== Run at %Y-%m-%d %H:%M:%S ===", _tm); else snprintf(_th, sizeof(_th), "=== Run started ==="); fputs(_th, out); fputs("\n", out); output_append_colored_line(_th);
            // use PTY to run command interactively
            int exitcode = run_command_in_pty(shell, out);
            if (exitcode != -1) {
                if (out) { fflush(out); fsync(fileno(out)); }
                if (out) fclose(out);
                // force reload of on-disk log when rendering
                E.output_last_mtime = 0;
                // show output pane in-editor (and keep log file too)
                E.output_visible = 1; if (E.output_n > 0) { E.output_scroll = 0; E.output_sel = 0; }
                editorRefreshScreen();
                snprintf(E.msg, sizeof(E.msg), "Command finished (exit %d)", exitcode); E.msg_time = time(NULL);
                return exitcode;
            } else {
                char emsg[4096]; snprintf(emsg, sizeof(emsg), "Failed to run: %s", shell);
                if (out) { fputs(emsg, out); fputs("\n", out); fflush(out); fsync(fileno(out)); }
                if (out) fclose(out);
                // also surface the error in the in-editor output pane
                output_clear();
                output_append_colored_line(emsg);
                // ensure any later render picks up the on-disk log
                E.output_last_mtime = 0;
                E.output_visible = 1; E.output_scroll = 0; E.output_sel = 0; editorRefreshScreen();
                snprintf(E.msg, sizeof(E.msg), "%s", emsg); E.msg_time = time(NULL); return -1;
            }
        } else {
            char emsg[256]; snprintf(emsg, sizeof(emsg), "Failed to create or open 'config/last_build.log': %s", strerror(errno));
            output_clear();
            output_append_colored_line(emsg);
            E.output_last_mtime = 0;
            E.output_visible = 1; E.output_scroll = 0; E.output_sel = 0; editorRefreshScreen();
            snprintf(E.msg, sizeof(E.msg), "%s", emsg); E.msg_time = time(NULL); return -1;
        }
    }

    // be forgiving: allow commands prefixed with ':' and optional space
    while (i < L && (cmd[i] == ':' || cmd[i] == ' ')) i++;

    /* Support short build/run commands typed into the command buffer and executed on Enter:
       - 'b' or 'B' : build (same as invoking Build flow)
       - 'br' or 'BR' : build then run (same as BR)
       - 'r' or 'R' : run only
       These are only processed when the user presses Enter (so typing is safe).
    */
    /* Only accept uppercase B/BR/R as explicit build/run commands (case-sensitive).
       Lowercase letters will be treated as normal typed commands as you expect. */
    if (i < L && cmd[i] == 'B') {
        // check for 'BR'
        int start = i; i++;
        int do_run_after = 0;
        if (i < L && cmd[i] == 'R') { do_run_after = 1; i++; }
        // consume optional trailing letters and whitespace
        while (i < L && isalpha((unsigned char)cmd[i])) i++;
        while (i < L && cmd[i] == ' ') i++;
        handle_build_run(do_run_after);
    }
    else if (i < L && (cmd[i] == 'R' || strncasecmp(&cmd[i], "run", 3) == 0)) {
        // run-only command: accept uppercase 'R' or the word 'run' (case-insensitive)
        if (cmd[i] == 'R') { i++; }
        else { i += 3; } // consumed 'run'
        while (i < L && isalpha((unsigned char)cmd[i])) i++; // consume any trailing letters (safety)
        while (i < L && cmd[i] == ' ') i++;
        const char *arg = NULL; char argbuf[1024] = {0};
        if (i < L) {
            // copy remainder as argument (trim leading/trailing spaces)
            int j = 0; while (i < L && cmd[i] == ' ') i++;
            while (i < L && j < (int)sizeof(argbuf)-1) { argbuf[j++] = cmd[i++]; }
            argbuf[j] = '\0'; for (int k = j-1; k >= 0 && isspace((unsigned char)argbuf[k]); k--) argbuf[k] = '\0';
            if (argbuf[0]) arg = argbuf;
        }
        handle_run_only(0, arg);
    }

    while (i < L) {
        // md <dirname> -> make directory (allow top-level placement)
        if (i + 1 < L && cmd[i] == 'm' && cmd[i+1] == 'd') {
            i += 2; while (i < L && cmd[i] == ' ') i++;
            int recursive = 0;
            // accept optional '-p' flag
            if (i + 1 < L && cmd[i] == '-') {
                int fstart = i; while (i < L && cmd[i] != ' ') i++; int flen = i - fstart;
                if (flen == 2 && cmd[fstart] == '-' && cmd[fstart+1] == 'p') recursive = 1;
                while (i < L && cmd[i] == ' ') i++;
            }
            int start = i; while (i < L && cmd[i] != ' ') i++; int tlen = i - start;
            if (tlen <= 0) { snprintf(E.msg, sizeof(E.msg), "Directory name required"); E.msg_time = time(NULL); continue; }
            char dname[512]; int sl = (tlen < (int)sizeof(dname) - 1) ? tlen : (int)sizeof(dname) - 1;
            memcpy(dname, &cmd[start], sl); dname[sl] = '\0';
            if (!recursive) {
                if (mkdir(dname, 0755) == 0) snprintf(E.msg, sizeof(E.msg), "Created directory %s", dname);
                else snprintf(E.msg, sizeof(E.msg), "mkdir failed: %s", strerror(errno));
                E.msg_time = time(NULL);
                continue;
            }
            // recursive (-p) behavior: create each component
            {
                char accum[1024] = {0};
                const char *s = dname;
                // preserve leading slash for absolute paths
                if (s[0] == '/') { strcpy(accum, "/"); }
                int failed = 0;
                while (*s) {
                    // skip any '/' separators
                    while (*s == '/') s++;
                    if (!*s) break;
                    const char *e = s; while (*e && *e != '/') e++;
                    int len = (int)(e - s);
                    if ((int)strlen(accum) + len + 2 >= (int)sizeof(accum)) { snprintf(E.msg, sizeof(E.msg), "Path too long"); failed = 1; break; }
                    if (!(strlen(accum) == 1 && accum[0] == '/')) strcat(accum, "/");
                    strncat(accum, s, len);
                    if (mkdir(accum, 0755) != 0) {
                        if (errno == EEXIST) { /* ok */ }
                        else { snprintf(E.msg, sizeof(E.msg), "mkdir failed: %s", strerror(errno)); failed = 1; break; }
                    }
                    s = e;
                }
                if (!failed) snprintf(E.msg, sizeof(E.msg), "Created directory %s", dname);
                E.msg_time = time(NULL);
                continue;
            }
        }
        // open file or manage files: fn <filename> | fn add <filename> | fn del <filename>
        if (i + 1 < L && cmd[i] == 'f' && cmd[i+1] == 'n') { i += 2;
            while (i < L && cmd[i] == ' ') i++;
            int start = i;
            // capture next token into tokenbuf
            while (i < L && cmd[i] != ' ') i++;
            int token_len = i - start;
            char token[64] = {0};
            if (token_len > 0) {
                int sl = (token_len < (int)sizeof(token) - 1) ? token_len : (int)sizeof(token) - 1;
                memcpy(token, &cmd[start], sl);
                token[sl] = '\0';
            }
            // if token is 'add' or 'del', parse next token as filename
            if (token_len > 0 && (strcmp(token, "add") == 0 || strcmp(token, "del") == 0)) {
                while (i < L && cmd[i] == ' ') i++;
                int fname_start = i;
                while (i < L && cmd[i] != ' ') i++;
                int flen = i - fname_start;
                if (flen > 0) {
                    char fname[512];
                    int sl = (flen < (int)sizeof(fname) - 1) ? flen : (int)sizeof(fname) - 1;
                    memcpy(fname, &cmd[fname_start], sl);
                    fname[sl] = '\0';
                    if (strcmp(token, "add") == 0) {
                        // create file on disk without opening it
                        FILE *cf = fopen(fname, "w");
                        if (cf) { fclose(cf); snprintf(E.msg, sizeof(E.msg), "Created %s", fname); E.msg_time = time(NULL); }
                        else { snprintf(E.msg, sizeof(E.msg), "Create failed: %s", strerror(errno)); E.msg_time = time(NULL); }
                    } else {
                        // del
                        if (access(fname, F_OK) == 0) {
                            if (unlink(fname) == 0) {
                    // if we deleted the currently open file, switch to last tab if available
                    if (E.filename && strcmp(E.filename, fname) == 0) {
                        if (NumTabs > 0) {
                            int target = NumTabs - 1;
                            if (switch_to_tab(target) == 0) {
                                snprintf(E.msg, sizeof(E.msg), "Deleted %s; switched to tab %d", fname, target+2);
                            } else {
                                // fallback: clear buffer
                                free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); snprintf(E.msg, sizeof(E.msg), "Deleted %s; opened empty buffer", fname);
                            }
                        } else {
                            free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); E.welcome_visible = 1; snprintf(E.msg, sizeof(E.msg), "Deleted %s; opened empty buffer", fname);
                        }
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "Deleted %s", fname);
                    }
                    E.msg_time = time(NULL);
                } else { snprintf(E.msg, sizeof(E.msg), "Delete failed: %s", strerror(errno)); E.msg_time = time(NULL); }
                        } else {
                            snprintf(E.msg, sizeof(E.msg), "No such file %s", fname); E.msg_time = time(NULL);
                        }
                    }
                } else {
                    snprintf(E.msg, sizeof(E.msg), "Filename required"); E.msg_time = time(NULL);
                }
                continue;
            }

            

            // otherwise treat token as a filename (rename if current file has name, else error)
            if (token_len > 0) {
                char fname[512];
                int sl = (token_len < (int)sizeof(fname) - 1) ? token_len : (int)sizeof(fname) - 1;
                memcpy(fname, &cmd[start], sl);
                fname[sl] = '\0';
                if (E.filename && E.filename[0]) {
                    // has filename: rename to fname after confirmation
                    char __tmpbuf[128]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mRename\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mto\x1b[0m \x1b[96m'%s'\x1b[0m? \x1b[92m(y)\x1b[0m\x1b[91m(N)\x1b[0m: ", E.filename, fname);
                    if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                    int y = 0; while (1) { int k2 = readKey(); if (k2 == -1) continue; if (k2=='y' || k2=='Y') { y = 1; break; } if (k2=='n' || k2=='N' || k2==27) { y = 0; break; } }
                    __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    if (y) {
                        // rename: if file exists on disk, rename it; update E.filename
                        if (access(E.filename, F_OK) == 0) {
                            if (rename(E.filename, fname) == 0) {
                                free(E.filename); E.filename = strdup(fname);
                                set_language_from_filename(fname);
                                snprintf(E.msg, sizeof(E.msg), "Renamed to %s", fname);
                            } else {
                                snprintf(E.msg, sizeof(E.msg), "Rename failed: %s", strerror(errno));
                            }
                        } else {
                            // file not on disk, just update name
                            free(E.filename); E.filename = strdup(fname);
                            set_language_from_filename(fname);
                            snprintf(E.msg, sizeof(E.msg), "Renamed to %s", fname);
                        }
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "Rename cancelled");
                    }
                    E.msg_time = time(NULL);
                } else {
                    // no filename: set the filename for this buffer
                    free(E.filename); E.filename = strdup(fname);
                    set_language_from_filename(fname);
                    // mark as unsaved
                    free(E.saved_snapshot); E.saved_snapshot = NULL;
                    snprintf(E.msg, sizeof(E.msg), "File named %s", fname);
                    E.msg_time = time(NULL);
                }
                continue;
            }
        }

        // opent <file> -> open file in a new tab (create if missing)
        if (i + 4 < L && strncmp(&cmd[i], "opent", 5) == 0) {
            char next = (i + 5 < L) ? cmd[i+5] : '\0';
            if (!(next == '\0' || next == ' ')) { i++; continue; }
            i += 5; while (i < L && cmd[i] == ' ') i++;
            int start = i; while (i < L && cmd[i] != ' ') i++; int tlen = i - start;
            if (tlen <= 0) { snprintf(E.msg, sizeof(E.msg), "Filename required"); E.msg_time = time(NULL); continue; }
            char fname[512]; int sl = (tlen < (int)sizeof(fname)-1) ? tlen : (int)sizeof(fname)-1; memcpy(fname, &cmd[start], sl); fname[sl] = '\0';
            // debug: show parsed filename and existence
            snprintf(E.msg, sizeof(E.msg), "opent -> '%s' (exists=%d)", fname, access(fname, F_OK) == 0); E.msg_time = time(NULL);
            // if file exists, prompt
            if (access(fname, F_OK) == 0) {
                // file exists: prompt open / create duplicate / cancel
                char __tmpbuf[128]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) {
                    int __wn = __tmpn;
                    if (__wn >= (int)sizeof(__tmpbuf)) __wn = (int)sizeof(__tmpbuf) - 1;
                    if (__wn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__wn);
                }
                int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mFile\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mexists\x1b[0m. \x1b[92m(o) Open\x1b[0m / \x1b[94m(d) Create duplicate\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", fname);
                if (pn > 0) {
                    int __wn = pn;
                    if (__wn >= (int)sizeof(__tmpbuf)) __wn = (int)sizeof(__tmpbuf) - 1;
                    if (__wn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__wn);
                }
                int resp = 0;
                while (1) {
                    int kk = readKey(); if (kk == -1) continue;
                    if (kk == 'o' || kk == 'O') { resp = 'o'; break; }
                    if (kk == 'd' || kk == 'D') { resp = 'd'; break; }
                    if (kk == 'c' || kk == 'C' || kk == 27) { resp = 'c'; break; }
                }
                __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                if (resp == 'c') { snprintf(E.msg, sizeof(E.msg), "Open cancelled"); E.msg_time = time(NULL); continue; }
                if (resp == 'd') {
                    // create duplicate: find available name like fname1.ext, fname2.ext, etc.
                    char base[512]; char ext[64] = {0};
                    char *dot = strrchr(fname, '.');
                    if (dot) {
                        int blen = dot - fname;
                        if (blen > 0) memcpy(base, fname, blen); base[blen] = '\0';
                        strcpy(ext, dot);
                    } else {
                        strcpy(base, fname);
                    }
                    char newname[2048];
                    int num = 1;
                    while (1) {
                        if (ext[0]) snprintf(newname, sizeof(newname), "%s%d%s", base, num, ext);
                        else snprintf(newname, sizeof(newname), "%s%d", base, num);
                        if (access(newname, F_OK) != 0) break;
                        num++;
                        if (num > 100) { snprintf(E.msg, sizeof(E.msg), "Too many duplicates"); E.msg_time = time(NULL); continue; }
                    }
                    // set fname to newname
                    strcpy(fname, newname);
                    // fall through to create
                }
                // resp == 'o': fall through to open existing
            } else {
                // doesn't exist: will create
            }
            // check if it's a directory
            struct stat st;
            if (stat(fname, &st) == 0 && S_ISDIR(st.st_mode)) {
                // open directory in browser
                char orig_cwd[512];
                if (getcwd(orig_cwd, sizeof(orig_cwd)) == NULL) {
                    snprintf(E.msg, sizeof(E.msg), "getcwd failed"); E.msg_time = time(NULL); continue;
                }
                if (chdir(fname) != 0) {
                    snprintf(E.msg, sizeof(E.msg), "Cannot access directory %s: %s", fname, strerror(errno)); E.msg_time = time(NULL); continue;
                }
                show_file_browser();
                chdir(orig_cwd);
                continue;
            }
            // Create a new tab with the file without modifying the current buffer
            open_file_in_new_tab(fname);
            snprintf(E.msg, sizeof(E.msg), "opent: opened %s", fname); E.msg_time = time(NULL);
            continue;
        }

        // open <file> -> open in current window but move current file to a new tab
        if (i + 3 < L && strncmp(&cmd[i], "open", 4) == 0) {
            char next = (i + 4 < L) ? cmd[i+4] : '\0'; if (!(next == '\0' || next == ' ')) { i++; continue; }
            i += 4; while (i < L && cmd[i] == ' ') i++;
            int start = i; while (i < L && cmd[i] != ' ') i++; int tlen = i - start;
            if (tlen <= 0) { snprintf(E.msg, sizeof(E.msg), "Filename required"); E.msg_time = time(NULL); continue; }
            char fname[512]; int sl = (tlen < (int)sizeof(fname)-1) ? tlen : (int)sizeof(fname)-1; memcpy(fname, &cmd[start], sl); fname[sl] = '\0';
            // debug: show parsed filename and existence
            snprintf(E.msg, sizeof(E.msg), "open -> '%s' (exists=%d)", fname, access(fname, F_OK) == 0); E.msg_time = time(NULL);
            // check if it's a directory
            struct stat st;
            if (stat(fname, &st) == 0 && S_ISDIR(st.st_mode)) {
                // open directory in browser
                char orig_cwd[512];
                if (getcwd(orig_cwd, sizeof(orig_cwd)) == NULL) {
                    snprintf(E.msg, sizeof(E.msg), "getcwd failed"); E.msg_time = time(NULL); continue;
                }
                if (chdir(fname) != 0) {
                    snprintf(E.msg, sizeof(E.msg), "Cannot access directory %s: %s", fname, strerror(errno)); E.msg_time = time(NULL); continue;
                }
                show_file_browser();
                chdir(orig_cwd);
                continue;
            }
            // push current HOME snapshot to front, switch to home, and open requested file into home
            push_home_to_front();
            switch_to_home();
            // then open requested file into current editor
            if (access(fname, F_OK) == 0) {
                // file exists: prompt open / create duplicate / cancel
                char __tmpbuf[128]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) {
                    int __wn = __tmpn;
                    if (__wn >= (int)sizeof(__tmpbuf)) __wn = (int)sizeof(__tmpbuf) - 1;
                    if (__wn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__wn);
                }
                int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mFile\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mexists\x1b[0m. \x1b[92m(o) Open\x1b[0m / \x1b[94m(d) Create duplicate\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", fname);
                if (pn > 0) {
                    int __wn = pn;
                    if (__wn >= (int)sizeof(__tmpbuf)) __wn = (int)sizeof(__tmpbuf) - 1;
                    if (__wn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__wn);
                }
                int resp = 0;
                while (1) {
                    int kk = readKey(); if (kk == -1) continue;
                    if (kk == 'o' || kk == 'O') { resp = 'o'; break; }
                    if (kk == 'd' || kk == 'D') { resp = 'd'; break; }
                    if (kk == 'c' || kk == 'C' || kk == 27) { resp = 'c'; break; }
                }
                __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                if (resp == 'c') { snprintf(E.msg, sizeof(E.msg), "Open cancelled"); E.msg_time = time(NULL); continue; }
                if (resp == 'd') {
                    // create duplicate: find available name like fname1.ext, fname2.ext, etc.
                    char base[512]; char ext[64] = {0};
                    char *dot = strrchr(fname, '.');
                    if (dot) {
                        int blen = dot - fname;
                        if (blen > 0) memcpy(base, fname, blen); base[blen] = '\0';
                        strcpy(ext, dot);
                    } else {
                        strcpy(base, fname);
                    }
                    char newname[2048];
                    int num = 1;
                    while (1) {
                        if (ext[0]) snprintf(newname, sizeof(newname), "%s%d%s", base, num, ext);
                        else snprintf(newname, sizeof(newname), "%s%d", base, num);
                        if (access(newname, F_OK) != 0) break;
                        num++;
                        if (num > 100) { snprintf(E.msg, sizeof(E.msg), "Too many duplicates"); E.msg_time = time(NULL); continue; }
                    }
                    // create new file with newname
                    FILE *nf = fopen(newname, "w");
                    if (nf) { fclose(nf); free(E.filename); E.filename = strdup(newname); free(E.saved_snapshot); E.saved_snapshot = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); snprintf(E.msg, sizeof(E.msg), "Created duplicate %s", newname); E.msg_time = time(NULL); E.welcome_visible = 0; }
                    else { snprintf(E.msg, sizeof(E.msg), "Create duplicate failed: %s", strerror(errno)); E.msg_time = time(NULL); }
                    continue;
                }
                // resp == 'o': fall through to open existing
            }
            FILE *f = fopen(fname, "r");
            if (!f) {
                // file doesn't exist: create it
                free(E.filename); E.filename = strdup(fname); free(E.saved_snapshot); E.saved_snapshot = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); snprintf(E.msg, sizeof(E.msg), "New file %s", fname); E.msg_time = time(NULL); E.welcome_visible = 0;
                /* ensure new file opens at top */
                E.cy = 0; E.cx = 0; E.row_offset = 0; E.col_offset = 0; editorScroll();
            } else {
                fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                char *buf = malloc(sz + 1); if (buf) { fread(buf, 1, sz, f); buf[sz] = '\0'; load_buffer_from_string(buf); free(buf); free(E.filename); E.filename = strdup(fname); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); snprintf(E.msg, sizeof(E.msg), "Opened %s", fname); E.msg_time = time(NULL); E.welcome_visible = 0;
                    /* ensure opened file shows from the top */
                    E.cy = 0; E.cx = 0; E.row_offset = 0; E.col_offset = 0; editorScroll();
                }
                fclose(f);
            }
            // debug: confirm push succeeded (num tabs)
            snprintf(E.msg, sizeof(E.msg), "open: pushed (tabs=%d)", NumTabs); E.msg_time = time(NULL);
            continue;
        }
        if (i + 1 < L && strncmp(&cmd[i], "cl", 2) == 0) {
            i += 2;
            if (!isdigit((unsigned char)cmd[i])) {
                // copy current line
                copy_line_n(E.cy + 1); continue;
            } else {
                int *list = NULL; int cnt = 0;
                int starti = i;
                if (parse_line_list(cmd, &i, &list, &cnt) == 0 && cnt > 0) {
                    copy_lines_list(list, cnt);
                    free(list);
                } else {
                    i = starti; // rollback to single number
                    int num = parse_number_at(cmd, &i);
                    if (num > 0) copy_line_n(num);
                }
                continue;
            }
        }
        if (i + 2 < L && strncmp(&cmd[i], "plf", 3) == 0) {
            i += 3; int num = parse_number_at(cmd, &i); if (num > 0) { paste_at_line_n_front(num); } continue;
        }
        if (i + 1 < L && strncmp(&cmd[i], "pl", 2) == 0) {
            i += 2; int num = parse_number_at(cmd, &i); if (num > 0) { paste_at_line_n(num); } continue;
        }
        // dall -> delete all lines (clear buffer)
        if (i + 3 < L && strncmp(&cmd[i], "dall", 4) == 0) {
            i += 4;
            /* snapshot current buffer before destructive clear so undo can restore the full content */
            save_snapshot();
            editorFreeRows();
            editorAppendRow("");
            /* Do not update E.saved_snapshot here; leave saved state as-is so the buffer is marked unsaved after clearing */
            E.cy = 0; E.cx = 0;
            /* snapshot after clearing so undo restores the buffer before this clear */
            save_snapshot();
            snprintf(E.msg, sizeof(E.msg), "Deleted all lines"); E.msg_time = time(NULL);
            continue;
        }

        // dl<N>w -> delete last N words on current line
        if (i + 2 < L && strncmp(&cmd[i], "dl", 2) == 0) {
            int save_i = i;
            i += 2;
            int num = parse_number_at(cmd, &i);
            if (num > 0 && i < L && cmd[i] == 'w') {
                i++; // consume 'w'
                save_snapshot(); delete_last_words(num);
                continue;
            }
            i = save_i; // rollback
        }
        // df<N>w -> delete first N words on current line
        if (i + 2 < L && strncmp(&cmd[i], "df", 2) == 0) {
            int save_i = i;
            i += 2;
            int num = parse_number_at(cmd, &i);
            if (num > 0 && i < L && cmd[i] == 'w') {
                i++; // consume 'w'
                save_snapshot(); delete_first_words(num);
                continue;
            }
            i = save_i; // rollback
        }

        if (i + 1 < L && strncmp(&cmd[i], "dl", 2) == 0) {
            i += 2;
            if (!isdigit((unsigned char)cmd[i])) {
                // delete current line
                save_snapshot(); delete_line_at(E.cy); continue;
            } else {
                int *list = NULL; int cnt = 0;
                int starti = i;
                if (parse_line_list(cmd, &i, &list, &cnt) == 0 && cnt > 0) {
                    qsort(list, cnt, sizeof(int), cmp_desc);
                    save_snapshot();
                    // delete each line
                    for (int k = 0; k < cnt; k++) {
                        int ln = list[k];
                        if (ln < 1 || ln > E.numrows) continue;
                        delete_line_at(ln - 1);
                    }
                    free(list);
                } else {
                    i = starti; // rollback to single number
                    int num = parse_number_at(cmd, &i);
                    if (num >= 1 && num <= E.numrows) {
                        save_snapshot();
                        int idx = num - 1;
                        char *row = E.rows[idx];
                        int len = (int)strlen(row);
                        int is_whitespace = 1;
                        for (int j = 0; j < len; j++) {
                            if (row[j] != ' ' && row[j] != '\t' && row[j] != '\r' && row[j] != '\n') {
                                is_whitespace = 0;
                                break;
                            }
                        }
                        if (is_whitespace) {
                            delete_line_at(idx);
                        } else {
                            row[0] = '\0';
                            if (E.cy == idx) E.cx = 0;
                        }
                    }
                }
                continue;
            }
        }
        if (i + 1 < L && strncmp(&cmd[i], "jl", 2) == 0) {
            char next = (i + 2 < L) ? cmd[i+2] : '\0';
            if (!(next == '\0' || next == ' ' || isdigit((unsigned char)next))) { i++; continue; }
            i += 2; int starti = i; int num = parse_number_at(cmd, &i);
            if (num >= 1 && num <= E.numrows) {
                /* ensure strict: after number must be end or space */
                if (i == starti || cmd[i] == '\0' || cmd[i] == ' ') {
                        E.cy = num - 1;
                        // move to first non-whitespace on the target line (front of statement)
                        {
                            char *trow = E.rows[E.cy];
                            int first_non = 0; while (trow[first_non] && (trow[first_non] == ' ' || trow[first_non] == '\t')) first_non++;
                            int rowlen = (int)strlen(trow);
                            if (first_non > rowlen) first_non = rowlen;
                            E.cx = first_non;
                        }
                        editorScroll(); snprintf(E.msg, sizeof(E.msg), "Jumped to line %d", num); E.msg_time = time(NULL);
                }
            }
            continue;
        }

        // tabs - list tabs
        if (i + 3 < L && strncmp(&cmd[i], "tabs", 4) == 0) {
            i += 4;
            // build a small list into E.msg (truncate if needed). Include home as tab 1, then stored tabs 2..N+1
            char buf[256]; buf[0] = '\0'; int p = 0;
            // home tab
            const char *hname = (CurTab == -1) ? (E.filename ? E.filename : "(No Name)") : (HaveHomeTab && HomeTab.name ? HomeTab.name : "(No Name)");
            int n = snprintf(buf + p, sizeof(buf) - p, "1:%s ", hname); if (n > 0) p += n;
            if (Tabs) {
                for (int t = 0; t < NumTabs; t++) {
                    int n2 = snprintf(buf + p, sizeof(buf) - p, "%d:%s ", t+2, Tabs[t].name ? Tabs[t].name : "");
                    if (n2 <= 0) break; p += n2; if (p >= (int)sizeof(buf)-20) break;
                }
            }
            if (p == 0) { snprintf(E.msg, sizeof(E.msg), "No tabs"); }
            else { snprintf(E.msg, sizeof(E.msg), "%s", buf); }
            E.msg_time = time(NULL);
            continue;
        }

        // browse - open file browser overlay. Allow single-letter 'b' as alias (e.g. ':b')
        if ((i + 4 < L && strncmp(&cmd[i], "browse", 6) == 0) || (cmd[i] == 'b' && (i+1 >= L || cmd[i+1] == ' '))) {
            if (cmd[i] == 'b') i += 1; else i += 6; show_file_browser(); continue;
        }

        // log - show output pane
        if (i + 2 < L && strncmp(&cmd[i], "log", 3) == 0) {
            i += 3;
            E.output_visible = 1;
            if (E.output_n > 0) { E.output_scroll = 0; E.output_sel = 0; }
            editorRefreshScreen();
            continue;
        }

        // cd <path> - change directory
        if (i + 1 < L && strncmp(&cmd[i], "cd", 2) == 0) {
            char next = (i + 2 < L) ? cmd[i+2] : '\0';
            if (!(next == '\0' || next == ' ')) { i++; continue; }
            i += 2; while (i < L && cmd[i] == ' ') i++;
            if (i >= L) { snprintf(E.msg, sizeof(E.msg), "cd: missing path"); E.msg_time = time(NULL); continue; }
            char path[512]; int pidx = 0;
            while (i < L && pidx < (int)sizeof(path)-1) { path[pidx++] = cmd[i++]; }
            path[pidx] = '\0';
            if (chdir(path) == 0) {
                char cwd[512]; getcwd(cwd, sizeof(cwd));
                snprintf(E.msg, sizeof(E.msg), "Changed directory to %s", cwd);
            } else {
                snprintf(E.msg, sizeof(E.msg), "cd failed: %s", strerror(errno));
            }
            E.msg_time = time(NULL);
            continue;
        }

        // help - toggle help overlay
        if (i + 3 < L && strncmp(&cmd[i], "help", 4) == 0) { i += 4; E.welcome_visible = 0; E.help_visible = 1; E.help_scroll = 0; continue; }

        // tabc <N> - close tab N (1-based). Allow compact forms like "tabc1" or "tabc 1" etc.
        // tabc all - close all tabs with prompts for unsaved changes
        if (i + 3 < L && strncmp(&cmd[i], "tabc", 4) == 0) {
            char next = (i + 4 < L) ? cmd[i+4] : '\0'; if (!(next == '\0' || next == ' ' || isdigit((unsigned char)next) || next == 'a')) { i++; continue; }
            i += 4; while (i < L && cmd[i] == ' ') i++; int start = i;
            // check if "all"
            if (i + 2 < L && strncmp(&cmd[i], "all", 3) == 0 && (i + 3 >= L || cmd[i+3] == ' ' || cmd[i+3] == '\0')) {
                i += 3;
                // tabc all: close all tabs, prompting for unsaved changes
                int cancelled = 0;
                // First, check home tab (tab 1)
                if (CurTab == -1) {
                    // home is current
                    char *cur = serialize_buffer();
                    int needs_save = 1;
                    if (E.saved_snapshot) {
                        if (strcmp(cur ? cur : "", E.saved_snapshot ? E.saved_snapshot : "") == 0) needs_save = 0;
                    }
                    free(cur);
                    if (needs_save) {
                        char __tmpbuf[512]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        const char *tabname = E.filename ? E.filename : "(No Name)";
                        // truncate tabname if too long to fit in buffer
                        char tabname_buf[256]; if (strlen(tabname) > sizeof(tabname_buf)-3) { memcpy(tabname_buf, tabname, sizeof(tabname_buf)-4); tabname_buf[sizeof(tabname_buf)-4] = '.'; tabname_buf[sizeof(tabname_buf)-3] = '.'; tabname_buf[sizeof(tabname_buf)-2] = '.'; tabname_buf[sizeof(tabname_buf)-1] = '\0'; tabname = tabname_buf; }
                        int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mTab\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mhas unsaved changes\x1b[0m. \x1b[92m(s) Save\x1b[0m / \x1b[94m(d) Discard\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tabname);
                        if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                        int resp = 0;
                        while (1) {
                            int k = readKey(); if (k == -1) continue;
                            if (k == 's' || k == 'S') { resp = 's'; break; }
                            if (k == 'd' || k == 'D') { resp = 'd'; break; }
                            if (k == 'c' || k == 'C' || k == 27) { resp = 'c'; break; }
                        }
                        __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        if (resp == 'c') { cancelled = 1; }
                        if (resp == 's') { save_to_file(NULL); }
                        // resp == 'd': discard, do nothing
                    }
                } else {
                    // home is not current, check if it has unsaved changes
                    if (HaveHomeTab) {
                        int needs_save = 1;
                        if (HomeTab.saved_snapshot) {
                            if (strcmp(HomeTab.snapshot ? HomeTab.snapshot : "", HomeTab.saved_snapshot ? HomeTab.saved_snapshot : "") == 0) needs_save = 0;
                        }
                        if (needs_save) {
                            char __tmpbuf[512]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                            if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                            const char *tabname = HomeTab.name && HomeTab.name[0] ? HomeTab.name : "(No Name)";
                            // truncate tabname if too long to fit in buffer
                            char tabname_buf[256]; if (strlen(tabname) > sizeof(tabname_buf)-3) { memcpy(tabname_buf, tabname, sizeof(tabname_buf)-4); tabname_buf[sizeof(tabname_buf)-4] = '.'; tabname_buf[sizeof(tabname_buf)-3] = '.'; tabname_buf[sizeof(tabname_buf)-2] = '.'; tabname_buf[sizeof(tabname_buf)-1] = '\0'; tabname = tabname_buf; }
                            int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mTab\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mhas unsaved changes\x1b[0m. \x1b[92m(s) Save\x1b[0m / \x1b[94m(d) Discard\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tabname);
                            if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                            int resp = 0;
                            while (1) {
                                int k = readKey(); if (k == -1) continue;
                                if (k == 's' || k == 'S') { resp = 's'; break; }
                                if (k == 'd' || k == 'D') { resp = 'd'; break; }
                                if (k == 'c' || k == 'C' || k == 27) { resp = 'c'; break; }
                            }
                            __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                            if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                            if (resp == 'c') { cancelled = 1; }
                            if (resp == 's') {
                                // save home tab: temporarily switch to it, save, switch back
                                int orig_cur = CurTab;
                                switch_to_home();
                                save_to_file(NULL);
                                if (orig_cur >= 0) switch_to_tab(orig_cur);
                            }
                            // resp == 'd': discard, do nothing
                        }
                    }
                }
                // Now check each stored tab
                for (int t = 0; t < NumTabs && !cancelled; t++) {
                    int needs_save = 1;
                    if (Tabs[t].saved_snapshot) {
                        if (strcmp(Tabs[t].snapshot ? Tabs[t].snapshot : "", Tabs[t].saved_snapshot ? Tabs[t].saved_snapshot : "") == 0) needs_save = 0;
                    }
                    if (needs_save) {
                        char __tmpbuf[512]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        const char *tabname = Tabs[t].name && Tabs[t].name[0] ? Tabs[t].name : "(No Name)";
                        // truncate tabname if too long to fit in buffer
                        char tabname_buf[256]; if (strlen(tabname) > sizeof(tabname_buf)-3) { memcpy(tabname_buf, tabname, sizeof(tabname_buf)-4); tabname_buf[sizeof(tabname_buf)-4] = '.'; tabname_buf[sizeof(tabname_buf)-3] = '.'; tabname_buf[sizeof(tabname_buf)-2] = '.'; tabname_buf[sizeof(tabname_buf)-1] = '\0'; tabname = tabname_buf; }
                        int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mTab\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mhas unsaved changes\x1b[0m. \x1b[92m(s) Save\x1b[0m / \x1b[94m(d) Discard\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tabname);
                        if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                        int resp = 0;
                        while (1) {
                            int k = readKey(); if (k == -1) continue;
                            if (k == 's' || k == 'S') { resp = 's'; break; }
                            if (k == 'd' || k == 'D') { resp = 'd'; break; }
                            if (k == 'c' || k == 'C' || k == 27) { resp = 'c'; break; }
                        }
                        __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        if (resp == 'c') { cancelled = 1; }
                        if (resp == 's') {
                            // save this tab: temporarily switch to it, save, switch back
                            int orig_cur = CurTab;
                            switch_to_tab(t);
                            save_to_file(NULL);
                            if (orig_cur >= 0 && orig_cur != t) switch_to_tab(orig_cur);
                            else if (orig_cur == -1) switch_to_home();
                        }
                        // resp == 'd': discard, do nothing
                    }
                }
                if (cancelled) {
                    snprintf(E.msg, sizeof(E.msg), "Close all cancelled"); E.msg_time = time(NULL);
                    continue;
                }
                // Now close all tabs: free all stored tabs, clear home, switch to empty buffer
                if (Tabs) {
                    for (int t = 0; t < NumTabs; t++) free_tab(&Tabs[t]);
                    free(Tabs); Tabs = NULL;
                }
                NumTabs = 0; CurTab = -1;
                free_tab(&HomeTab); HaveHomeTab = 0;
                free(E.filename); E.filename = NULL;
                editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
                E.welcome_visible = 1; E.help_from_welcome = 0; E.browser_from_welcome = 0;
                snprintf(E.msg, sizeof(E.msg), "Closed all tabs"); E.msg_time = time(NULL);
                continue;
            }
            // parse number
            while (i < L && isdigit((unsigned char)cmd[i])) i++; int tlen = i - start;
            if (tlen <= 0) {
                // no number: close the currently-displayed tab (home==1 or stored)
                if (CurTab >= 0) {
                    int val = CurTab + 2; // displayed number
                    if (close_tab(CurTab) == 0) snprintf(E.msg, sizeof(E.msg), "Closed current tab %d", val);
                    else snprintf(E.msg, sizeof(E.msg), "Close failed for current tab %d", val);
                } else {
                    // closing home buffer
                    if (NumTabs > 0) {
                        int target = NumTabs - 1;
                        HomeTab = Tabs[target];
                        HaveHomeTab = 1;
                        NumTabs--;
                        CurTab = -1;
                        switch_to_home();
                        snprintf(E.msg, sizeof(E.msg), "Closed current buffer; promoted tab %d to home", target + 2);
                    } else {
                        // no other tabs: clear to empty buffer and show welcome page
                        free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
                        E.welcome_visible = 1;
                        free_tab(&HomeTab); HaveHomeTab = 0;
                        snprintf(E.msg, sizeof(E.msg), "Closed current buffer");
                    }
                }
                E.msg_time = time(NULL);
                continue;
            }
            int val = 0; for (int k = 0; k < tlen; k++) val = val * 10 + (cmd[start + k] - '0');
            if (val < 1 || val > NumTabs + 1) { snprintf(E.msg, sizeof(E.msg), "Bad tab %d", val); E.msg_time = time(NULL); continue; }
            if (val == 1) {
                // close home explicitly
                if (CurTab == -1) {
                    // same as no-number close above
                    if (NumTabs > 0) {
                        int target = NumTabs - 1;
                        HomeTab = Tabs[target];
                        HaveHomeTab = 1;
                        NumTabs--;
                        CurTab = -1;
                        switch_to_home();
                        snprintf(E.msg, sizeof(E.msg), "Closed tab 1");
                    } else {
                        free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); E.welcome_visible = 1; free_tab(&HomeTab); HaveHomeTab = 0; snprintf(E.msg, sizeof(E.msg), "Closed current buffer");
                    }
                } else {
                    // home not visible: promote first stored tab (if any) to home so tab numbering remains compact
                    if (NumTabs > 0) {
                        // move Tabs[0] into HomeTab
                        free_tab(&HomeTab);
                        HomeTab = Tabs[0];
                        HaveHomeTab = 1;
                        // shift remaining tabs left
                        for (int _j = 0; _j < NumTabs - 1; _j++) Tabs[_j] = Tabs[_j + 1];
                        NumTabs--;
                        // adjust current tab index: if user was viewing a stored tab, update its index
                        if (CurTab > 0) CurTab--; else if (CurTab == 0) { CurTab = -1; switch_to_home(); }
                        snprintf(E.msg, sizeof(E.msg), "Closed buffer 1 (home); promoted next tab to home");
                    } else {
                        free_tab(&HomeTab); HaveHomeTab = 0; snprintf(E.msg, sizeof(E.msg), "Closed buffer 1 (home)");
                    }
                }
                E.msg_time = time(NULL); continue;
            }
            // val > 1 -> stored tab index = val - 2
            if (close_tab(val - 2) == 0) snprintf(E.msg, sizeof(E.msg), "Closed tab %d", val);
            else snprintf(E.msg, sizeof(E.msg), "Close failed for tab %d", val);
            E.msg_time = time(NULL);
            continue;
        }

        // tab <N> - switch to tab N (1-based). Allow compact forms like "tab1" or "tab  1" etc.
        if (i + 2 < L && strncmp(&cmd[i], "tab", 3) == 0) {
            char next = (i + 3 < L) ? cmd[i+3] : '\0'; if (!(next == '\0' || next == ' ' || isdigit((unsigned char)next))) { i++; continue; }
            i += 3; while (i < L && cmd[i] == ' ') i++; int start = i; while (i < L && isdigit((unsigned char)cmd[i])) i++; int tlen = i - start;
            if (tlen <= 0) { snprintf(E.msg, sizeof(E.msg), "Tab number required"); E.msg_time = time(NULL); continue; }
            int val = 0; for (int k = 0; k < tlen; k++) val = val * 10 + (cmd[start + k] - '0');
            if (val < 1 || val > NumTabs + 1) { snprintf(E.msg, sizeof(E.msg), "Bad tab %d", val); E.msg_time = time(NULL); continue; }
            if (val == 1) {
                switch_to_home();
            } else {
                switch_to_tab(val - 2);
            }
            snprintf(E.msg, sizeof(E.msg), "Switched to tab %d", val); E.msg_time = time(NULL);
            continue;
        }
        if (i + 2 < L && strncmp(&cmd[i], "jfl", 3) == 0) {
            char next = (i + 3 < L) ? cmd[i+3] : '\0';
            if (next == '\0' || next == ' ' || isdigit((unsigned char)next)) {
                int idx = i + 3; while (idx < L && cmd[idx] == ' ') idx++;
                int num = parse_number_at(cmd, &idx);
                /* if there was a number, require that it be followed by space or end */
                if (num != -1 && !(idx == i+3 || cmd[idx] == '\0' || cmd[idx] == ' ')) { i++; continue; }
                int target = (num >= 1 && num <= E.numrows) ? num - 1 : E.cy;
                if (target < 0) target = 0;
                if (target >= E.numrows) target = E.numrows - 1;
                E.cy = target;
                // move to first non-whitespace on the target line (front)
                {
                    char *trow = E.rows[E.cy];
                    int first_non = 0; while (trow[first_non] && (trow[first_non] == ' ' || trow[first_non] == '\t')) first_non++;
                    int rowlen = (int)strlen(trow);
                    if (first_non > rowlen) first_non = rowlen;
                    E.cx = first_non;
                }
                editorScroll();
                snprintf(E.msg, sizeof(E.msg), "Jumped to line %d front", target + 1); E.msg_time = time(NULL);
                i = idx; continue;
            }
        }
        if (i + 2 < L && strncmp(&cmd[i], "jel", 3) == 0) {
            char next = (i + 3 < L) ? cmd[i+3] : '\0';
            if (next == '\0' || next == ' ' || isdigit((unsigned char)next)) {
                int idx = i + 3; while (idx < L && cmd[idx] == ' ') idx++;
                int num = parse_number_at(cmd, &idx);
                /* if there was a number, require that it be followed by space or end */
                if (num != -1 && !(idx == i+3 || cmd[idx] == '\0' || cmd[idx] == ' ')) { i++; continue; }
                int target = (num >= 1 && num <= E.numrows) ? num - 1 : E.cy;
                if (target < 0) target = 0;
                if (target >= E.numrows) target = E.numrows - 1;
                E.cy = target;
                E.cx = strlen(E.rows[target]);
                editorScroll();
                snprintf(E.msg, sizeof(E.msg), "Jumped to line %d end", target + 1); E.msg_time = time(NULL);
                i = idx; continue;
            }
        }
        if (i + 2 < L && strncmp(&cmd[i], "jml", 3) == 0) {
            char next = (i + 3 < L) ? cmd[i+3] : '\0';
            if (next == '\0' || next == ' ' || isdigit((unsigned char)next)) {
                int idx = i + 3;
                while (idx < L && cmd[idx] == ' ') idx++;
                int num = parse_number_at(cmd, &idx);
                int target = (num >= 1 && num <= E.numrows) ? num - 1 : E.cy;
                if (target < 0) target = 0;
                if (target >= E.numrows) target = E.numrows - 1;
                E.cy = target;
                E.cx = (int)(strlen(E.rows[target]) / 2);
                editorScroll();
                snprintf(E.msg, sizeof(E.msg), "Jumped to line %d middle", target + 1); E.msg_time = time(NULL);
                i = idx; continue;
            }
        }
        if (i + 1 < L && strncmp(&cmd[i], "ot", 2) == 0) {
            char next = (i + 2 < L) ? cmd[i+2] : '\0';
            if (!(next == '\0' || next == ' ')) { i++; continue; }
            i += 2;
            // Open embedded PTY terminal inside the editor
            run_embedded_terminal();
            continue;
        }
        // single-char tokens (strict: only trigger when followed by end or space)
        if (cmd[i] == 'u') {
            char next = (i + 1 < L) ? cmd[i+1] : '\0';
            if (next == '\0' || next == ' ') { undo_cmd(); i++; continue; } else { i++; continue; }
        }
        if (cmd[i] == 'r') {
            char next = (i + 1 < L) ? cmd[i+1] : '\0';
            if (next == '\0' || next == ' ') { redo_cmd(); i++; continue; } else { i++; continue; }
        }
        if (cmd[i] == 's') {
            char next = (i + 1 < L) ? cmd[i+1] : '\0';
            if (next == '\0' || next == ' ') {
                // prompt to confirm save
                char __tmpbuf[128]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1mSave file?\x1b[0m \x1b[92m(y)\x1b[0m\x1b[91m(N)\x1b[0m: ");
                if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                int y = 0; while (1) { int k2 = readKey(); if (k2 == -1) continue; if (k2=='y' || k2=='Y') { y = 1; break; } if (k2=='n' || k2=='N' || k2==27) { y = 0; break; } }
                __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                if (y) {
                    save_to_file(NULL);
                } else {
                    snprintf(E.msg, sizeof(E.msg), "Save cancelled"); E.msg_time = time(NULL);
                }
                i++; continue;
            } else { i++; continue; }
        }
        if (cmd[i] == 'p') {
            char next = (i + 1 < L) ? cmd[i+1] : '\0';
            if (next == '\0' || next == ' ') { paste_at_cursor(); i++; continue; } else { i++; continue; }
        }
        if (cmd[i] == 'q') {
            char next = (i + 1 < L) ? cmd[i+1] : '\0';
            if (!(next == '\0' || next == ' ')) { i++; continue; }
            // quit: check all tabs for unsaved changes, prompt individually
            int cancelled = 0;
            // First, check home tab (tab 1)
            if (CurTab == -1) {
                // home is current
                char *cur = serialize_buffer();
                int needs_save = 1;
                if (E.saved_snapshot) {
                    if (strcmp(cur ? cur : "", E.saved_snapshot ? E.saved_snapshot : "") == 0) needs_save = 0;
                }
                free(cur);
                if (needs_save) {
                    char __tmpbuf[512]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    const char *tabname = E.filename ? E.filename : "(No Name)";
                    // truncate tabname if too long to fit in buffer
                    char tabname_buf[256]; if (strlen(tabname) > sizeof(tabname_buf)-3) { memcpy(tabname_buf, tabname, sizeof(tabname_buf)-4); tabname_buf[sizeof(tabname_buf)-4] = '.'; tabname_buf[sizeof(tabname_buf)-3] = '.'; tabname_buf[sizeof(tabname_buf)-2] = '.'; tabname_buf[sizeof(tabname_buf)-1] = '\0'; tabname = tabname_buf; }
                    int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mTab\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mhas unsaved changes\x1b[0m. \x1b[92m(s) Save\x1b[0m / \x1b[94m(d) Discard\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tabname);
                    if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                    int resp = 0;
                    while (1) {
                        int k = readKey(); if (k == -1) continue;
                        if (k == 's' || k == 'S') { resp = 's'; break; }
                        if (k == 'd' || k == 'D') { resp = 'd'; break; }
                        if (k == 'c' || k == 'C' || k == 27) { resp = 'c'; break; }
                    }
                    __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    if (resp == 'c') { cancelled = 1; }
                    if (resp == 's') { save_to_file(NULL); }
                    // resp == 'd': discard, do nothing
                }
            } else {
                // home is not current, check if it has unsaved changes
                if (HaveHomeTab) {
                    int needs_save = 1;
                    if (HomeTab.saved_snapshot) {
                        if (strcmp(HomeTab.snapshot ? HomeTab.snapshot : "", HomeTab.saved_snapshot ? HomeTab.saved_snapshot : "") == 0) needs_save = 0;
                    }
                    if (needs_save) {
                        char __tmpbuf[512]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        const char *tabname = HomeTab.name && HomeTab.name[0] ? HomeTab.name : "(No Name)";
                        // truncate tabname if too long to fit in buffer
                        char tabname_buf[256]; if (strlen(tabname) > sizeof(tabname_buf)-3) { memcpy(tabname_buf, tabname, sizeof(tabname_buf)-4); tabname_buf[sizeof(tabname_buf)-4] = '.'; tabname_buf[sizeof(tabname_buf)-3] = '.'; tabname_buf[sizeof(tabname_buf)-2] = '.'; tabname_buf[sizeof(tabname_buf)-1] = '\0'; tabname = tabname_buf; }
                        int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mTab\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mhas unsaved changes\x1b[0m. \x1b[92m(s) Save\x1b[0m / \x1b[94m(d) Discard\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tabname);
                        if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                        int resp = 0;
                        while (1) {
                            int k = readKey(); if (k == -1) continue;
                            if (k == 's' || k == 'S') { resp = 's'; break; }
                            if (k == 'd' || k == 'D') { resp = 'd'; break; }
                            if (k == 'c' || k == 'C' || k == 27) { resp = 'c'; break; }
                        }
                        __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        if (resp == 'c') { cancelled = 1; }
                        if (resp == 's') {
                            // save home tab: temporarily switch to it, save, switch back
                            int orig_cur = CurTab;
                            switch_to_home();
                            save_to_file(NULL);
                            if (orig_cur >= 0) switch_to_tab(orig_cur);
                        }
                        // resp == 'd': discard, do nothing
                    }
                }
            }
            // Now check each stored tab
            if (Tabs) for (int t = 0; t < NumTabs && !cancelled; t++) {
                int needs_save = 1;
                if (t == CurTab) {
                    char *cur = serialize_buffer();
                    if (cur) {
                        if (E.saved_snapshot && strcmp(E.saved_snapshot, cur) == 0) needs_save = 0;
                        free(cur);
                    }
                } else {
                    if (Tabs[t].saved_snapshot) {
                        if (strcmp(Tabs[t].snapshot ? Tabs[t].snapshot : "", Tabs[t].saved_snapshot ? Tabs[t].saved_snapshot : "") == 0) needs_save = 0;
                    }
                }
                if (needs_save) {
                    char __tmpbuf[512]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    const char *tabname = Tabs[t].name && Tabs[t].name[0] ? Tabs[t].name : "(No Name)";
                    // truncate tabname if too long to fit in buffer
                    char tabname_buf[256]; if (strlen(tabname) > sizeof(tabname_buf)-3) { memcpy(tabname_buf, tabname, sizeof(tabname_buf)-4); tabname_buf[sizeof(tabname_buf)-4] = '.'; tabname_buf[sizeof(tabname_buf)-3] = '.'; tabname_buf[sizeof(tabname_buf)-2] = '.'; tabname_buf[sizeof(tabname_buf)-1] = '\0'; tabname = tabname_buf; }
                    int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mTab\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mhas unsaved changes\x1b[0m. \x1b[92m(s) Save\x1b[0m / \x1b[94m(d) Discard\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tabname);
                    if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                    int resp = 0;
                    while (1) {
                        int k = readKey(); if (k == -1) continue;
                        if (k == 's' || k == 'S') { resp = 's'; break; }
                        if (k == 'd' || k == 'D') { resp = 'd'; break; }
                        if (k == 'c' || k == 'C' || k == 27) { resp = 'c'; break; }
                    }
                    __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    if (resp == 'c') { cancelled = 1; }
                    if (resp == 's') {
                        if (t == CurTab) {
                            save_to_file(NULL);
                        } else {
                            // save this tab: temporarily switch to it, save, switch back
                            int orig_cur = CurTab;
                            switch_to_tab(t);
                            save_to_file(NULL);
                            if (orig_cur >= 0 && orig_cur != t) switch_to_tab(orig_cur);
                            else if (orig_cur == -1) switch_to_home();
                        }
                    }
                    // resp == 'd': discard, do nothing
                }
            }
            if (cancelled) {
                snprintf(E.msg, sizeof(E.msg), "Quit cancelled"); E.msg_time = time(NULL);
                i++; continue;
            }
            // Now quit: remove swap files for all tabs
            // remove swap for home
            if (E.filename) {
                char *base = strrchr(E.filename, '/');
                const char *b = base ? base + 1 : E.filename;
                char swapbuf[1024];
                if (base) snprintf(swapbuf, sizeof(swapbuf), "%.*s/.%s.swp", (int)(base - E.filename), E.filename, b);
                else snprintf(swapbuf, sizeof(swapbuf), ".%s.swp", b);
                unlink(swapbuf);
            }
            // remove swap for stored tabs
            if (Tabs) for (int t = 0; t < NumTabs; t++) {
                if (Tabs[t].name) {
                    char *base = strrchr(Tabs[t].name, '/');
                    const char *b = base ? base + 1 : Tabs[t].name;
                    char swapbuf[1024];
                    if (base) snprintf(swapbuf, sizeof(swapbuf), "%.*s/.%s.swp", (int)(base - Tabs[t].name), Tabs[t].name, b);
                    else snprintf(swapbuf, sizeof(swapbuf), ".%s.swp", b);
                    unlink(swapbuf);
                }
            }
            write(STDOUT_FILENO, "\x1b[2J", 4); write(STDOUT_FILENO, "\x1b[H", 3); disableRawMode(); exit(0);
        }
        if (i + 1 < L && strncmp(&cmd[i], "qs", 2) == 0) {
            char next = (i + 2 < L) ? cmd[i+2] : '\0';
            if (!(next == '\0' || next == ' ')) { i++; continue; }
            i += 2;
            // quit and save: save all unsaved tabs, then quit
            // First, save home tab if needed
            if (CurTab == -1) {
                // home is current
                char *cur = serialize_buffer();
                int needs_save = 1;
                if (E.saved_snapshot) {
                    if (strcmp(cur ? cur : "", E.saved_snapshot ? E.saved_snapshot : "") == 0) needs_save = 0;
                }
                free(cur);
                if (needs_save) save_to_file(NULL);
            } else {
                // home is not current, check if it has unsaved changes
                if (HaveHomeTab) {
                    int needs_save = 1;
                    if (HomeTab.saved_snapshot) {
                        if (strcmp(HomeTab.snapshot ? HomeTab.snapshot : "", HomeTab.saved_snapshot ? HomeTab.saved_snapshot : "") == 0) needs_save = 0;
                    }
                    if (needs_save) {
                        // save home tab: temporarily switch to it, save, switch back
                        int orig_cur = CurTab;
                        switch_to_home();
                        save_to_file(NULL);
                        if (orig_cur >= 0) switch_to_tab(orig_cur);
                    }
                }
            }
            // Now save each stored tab if needed
            if (Tabs) {
                for (int t = 0; t < NumTabs; t++) {
                    int needs_save = 1;
                    if (Tabs[t].saved_snapshot) {
                        if (strcmp(Tabs[t].snapshot ? Tabs[t].snapshot : "", Tabs[t].saved_snapshot ? Tabs[t].saved_snapshot : "") == 0) needs_save = 0;
                    }
                    if (needs_save) {
                        // save this tab: temporarily switch to it, save, switch back
                        int orig_cur = CurTab;
                        switch_to_tab(t);
                        save_to_file(NULL);
                        if (orig_cur >= 0 && orig_cur != t) switch_to_tab(orig_cur);
                        else if (orig_cur == -1) switch_to_home();
                    }
                }
            }
            // Now quit: remove swap files for all tabs
            // remove swap for home
            if (E.filename) {
                char *base = strrchr(E.filename, '/');
                const char *b = base ? base + 1 : E.filename;
                char swapbuf[1024];
                if (base) snprintf(swapbuf, sizeof(swapbuf), "%.*s/.%s.swp", (int)(base - E.filename), E.filename, b);
                else snprintf(swapbuf, sizeof(swapbuf), ".%s.swp", b);
                unlink(swapbuf);
            }
            // remove swap for stored tabs
            if (Tabs) for (int t = 0; t < NumTabs; t++) {
                if (Tabs[t].name) {
                    char *base = strrchr(Tabs[t].name, '/');
                    const char *b = base ? base + 1 : Tabs[t].name;
                    char swapbuf[1024];
                    if (base) snprintf(swapbuf, sizeof(swapbuf), "%.*s/.%s.swp", (int)(base - Tabs[t].name), Tabs[t].name, b);
                    else snprintf(swapbuf, sizeof(swapbuf), ".%s.swp", b);
                    unlink(swapbuf);
                }
            }
            write(STDOUT_FILENO, "\x1b[2J", 4); write(STDOUT_FILENO, "\x1b[H", 3); disableRawMode(); exit(0);
        }

        // unknown token: skip
        i++;
    }
    return 0;
}

static void editorInsertChar(int c) {
    if (E.cy < 0) E.cy = 0;
    if (E.cy >= E.numrows) {
        // create missing rows up to cy
        while (E.numrows <= E.cy) editorAppendRow("");
    }

    /* snapshot before the edit so an undo will restore the state prior to the typing group */
    maybe_snapshot(EDIT_KIND_INSERT);

    char *row = E.rows[E.cy];
    int len = strlen(row);
    if (E.cx < 0) E.cx = 0;
    if (E.cx > len) E.cx = len;

    char *newrow = malloc(len + 2 + 1); // +1 for new char, +1 for NUL
    if (!newrow) die("malloc");
    memcpy(newrow, row, E.cx);
    newrow[E.cx] = (char)c;
    memcpy(newrow + E.cx + 1, row + E.cx, len - E.cx + 1);

    free(E.rows[E.cy]);
    E.rows[E.cy] = newrow;
    E.cx++;
    editorScroll();
}

static void editorInsertNewline(void) {
    if (E.cy < 0) E.cy = 0;
    /* snapshot before the edit so undo restores pre-newline state */
    maybe_snapshot(EDIT_KIND_NEWLINE);
    if (E.cy >= E.numrows) {
        editorAppendRow("");
        E.cy = E.numrows - 1;
        E.cx = 0;
        editorScroll();
        return;
    }
    char *row = E.rows[E.cy];
    int len = strlen(row);
    if (E.cx < 0) E.cx = 0;
    if (E.cx > len) E.cx = len;

    // current row becomes left part
    char *left = strndup(row, E.cx);
    if (!left) die("strndup");
    // new row is right part
    char *right = strdup(row + E.cx);
    if (!right) die("strdup");

    free(E.rows[E.cy]);
    E.rows[E.cy] = left;
    editorInsertRow(E.cy + 1, right);
    free(right);

    E.cy++;
    // compute indentation for new row based on current (left) row
    int base_indent = compute_leading_spaces(E.rows[E.cy - 1]);
    int lasti = (int)strlen(E.rows[E.cy - 1]) - 1;
    while (lasti >= 0 && isspace((unsigned char)E.rows[E.cy - 1][lasti])) lasti--;
    char lastch = (lasti >= 0) ? E.rows[E.cy - 1][lasti] : '\0';
    int add = 0;
    if (lastch == '{' || lastch == ':') add = conf.indent_size;
    int new_indent = base_indent + add;
    if (new_indent < 0) new_indent = 0;
    char *ind = make_indent_string(new_indent);
    if (ind && *ind) {
        char *orig = E.rows[E.cy];
        int origlen = strlen(orig);
        int indlen = (int)strlen(ind);
        char *newrow = malloc(indlen + origlen + 1);
        if (newrow) {
            memcpy(newrow, ind, indlen);
            memcpy(newrow + indlen, orig, origlen + 1);
            free(E.rows[E.cy]);
            E.rows[E.cy] = newrow;
            E.cx = indlen;
        }
        free(ind);
    } else {
        E.cx = 0;
    }
    editorScroll();
}

static void editorDelChar(void) {
    if (E.cy < 0 || E.cy >= E.numrows) return;
    /* snapshot before deletion so undo will restore the state before the delete group */
    maybe_snapshot(EDIT_KIND_DELETE);
    char *row = E.rows[E.cy];
    int len = strlen(row);
    if (E.cx > 0) {
        // remove char to left
        memmove(row + E.cx - 1, row + E.cx, len - E.cx + 1);
        E.cx--;
    } else if (E.cx == 0 && E.cy > 0) {
        // join with previous row
        int prevlen = strlen(E.rows[E.cy - 1]);
        char *newrow = malloc(prevlen + len + 1);
        if (!newrow) die("malloc");
        memcpy(newrow, E.rows[E.cy - 1], prevlen);
        memcpy(newrow + prevlen, row, len + 1);
        free(E.rows[E.cy - 1]);
        E.rows[E.cy - 1] = newrow;
        // remove current row
        free(E.rows[E.cy]);
        memmove(&E.rows[E.cy], &E.rows[E.cy + 1], sizeof(char*) * (E.numrows - E.cy - 1));
        E.numrows--;
        E.cy--;
        E.cx = prevlen;
    }
    editorScroll();
    maybe_snapshot(EDIT_KIND_DELETE);
}

static int readKey(void) {
    char c;
    ssize_t nread = read(STDIN_FILENO, &c, 1);
    if (nread <= 0) return -1;
    if (c == 27) { // escape sequence
        // Wait briefly to see if there's more bytes (CSI sequence)
        fd_set rdset;
        struct timeval tv;
        FD_ZERO(&rdset);
        FD_SET(STDIN_FILENO, &rdset);
        tv.tv_sec = 0;
        tv.tv_usec = 100000; // 100ms
        int r = select(STDIN_FILENO + 1, &rdset, NULL, NULL, &tv);
        if (r <= 0) {
            return 27; // lone ESC
        }
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) <= 0) return 27;
        if (read(STDIN_FILENO, &seq[1], 1) <= 0) seq[1] = '\0';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                // Extended escape, read another
                if (read(STDIN_FILENO, &seq[2], 1) <= 0) seq[2] = '\0';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return 27;
    }
    return (int)(unsigned char)c;
}

static void editorScroll(void) {
    // compute visible content rows consistently with editorRefreshScreen
    // reserve 1 row for status; reserve an additional row if tab bar is shown
    int content_rows = E.screenrows - 1;
    if (conf.show_tab_bar) content_rows -= 1;
    if (content_rows < 1) content_rows = 1;
    if (E.cy < E.row_offset) {
        E.row_offset = E.cy;
    } else if (E.cy >= E.row_offset + content_rows) {
        E.row_offset = E.cy - content_rows + 1;
    }

    // Horizontal scrolling: ensure E.cx is within [col_offset, col_offset + avail - 1]
    int total_lines = (E.numrows > content_rows) ? E.numrows : content_rows;
    int ln_width = snprintf(NULL, 0, "%d", total_lines);
    if (ln_width < 1) ln_width = 1;
    int prefix_len = ln_width + 3;
    int avail = E.screencols - prefix_len;
    if (avail < 1) avail = 1;

    if (E.cx < E.col_offset) {
        E.col_offset = E.cx;
    } else if (E.cx >= E.col_offset + avail) {
        E.col_offset = E.cx - avail + 1;
    }
}

static void draw_help_overlay(void) {
    // hide cursor during help
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    // build a more compact, nicely padded help box (similar look to the file browser)
    const char *lines[] = {
        "\x1b[96mCommand Cheat Sheet\x1b[0m",
        "",
        "\x1b[96mFile & Tab Management\x1b[0m",
        "\x1b[93mfn <file>\x1b[0m                Name/rename current file",
        "\x1b[93mfn add <file>\x1b[0m            Create file on disk",
        "\x1b[93mfn del <file>\x1b[0m            Delete file",
        "\x1b[93mopen <file>\x1b[0m              Open in current window",
        "\x1b[93mopent <file>\x1b[0m             Open in new tab",
        "\x1b[93mtabs\x1b[0m                       List tabs",
        "\x1b[93mtab<N>\x1b[0m                     Switch to tab N",
        "\x1b[93mtabc<N>\x1b[0m                    Close tab N",
        "\x1b[93mtabc all\x1b[0m                   Close all tabs (prompts to save)",
        "\x1b[93mmd <dir>\x1b[0m                  Make directory",
        "\x1b[93mmd -p <dir>\x1b[0m               Make directory (recursive)",
        "\x1b[93mcd <path>\x1b[0m                Change directory",
        "",
        "\x1b[96mEditing\x1b[0m",
        "\x1b[93ms\x1b[0m                          Save",
        "\x1b[93mq\x1b[0m                          Quit (prompts to save)",
        "\x1b[93mqs\x1b[0m                         Quit & Save",
        "\x1b[93mu\x1b[0m                          Undo",
        "\x1b[93mr\x1b[0m                          Redo",
        "\x1b[93mcl<N>\x1b[0m                      Copy line(s) N or range",
        "\x1b[93mp\x1b[0m                          Paste at cursor",
        "\x1b[93mpl<N>\x1b[0m                      Paste at line N",
        "\x1b[93mplf<N>\x1b[0m                     Paste at front of line N",
        "\x1b[93mdall\x1b[0m                       Delete all lines",
        "\x1b[93mdl\x1b[0m                         Delete current line",
        "\x1b[93mdl<Range>\x1b[0m                  Clear text on range (remove empty lines)",
        "\x1b[93mdl<N>\x1b[0m                      Clear text on line N (remove if empty)",
        "\x1b[93mdl<N>w\x1b[0m                     Delete last N words",
        "\x1b[93mdf<N>w\x1b[0m                     Delete first N words",
        "",
        "\x1b[96mNavigation\x1b[0m",
        "\x1b[93mjl<N>\x1b[0m                      Jump to front of line N",
        "\x1b[93mjml<N>\x1b[0m                     Jump to middle of line N",
        "\x1b[93mjel<N>\x1b[0m                     Jump to end of line N",
        "",
        "\x1b[96mFile Browser\x1b[0m",
        "\x1b[93mb\x1b[0m                          File browser",
        "",
        "\x1b[96mTerminal\x1b[0m",
        "\x1b[93mot\x1b[0m                         Open Terminal",
        "Type \"\x1b[91mexit\x1b[0m\" into the terminal to close",
        "",
        "\x1b[96mHelp\x1b[0m",
        "\x1b[93mhelp\x1b[0m                       Toggle help overlay",
        "\x1b[93m?\x1b[0m                          Toggle help overlay",
        "",
        ""
    };
    int n = sizeof(lines) / sizeof(lines[0]);

    // compute content height
    int content_h = E.screenrows - 6; // leave margin
    if (content_h > n) content_h = n;
    // clamp scroll
    if (E.help_scroll < 0) E.help_scroll = 0;
    int max_scroll = n - content_h;
    if (max_scroll < 0) max_scroll = 0;
    if (E.help_scroll > max_scroll) E.help_scroll = max_scroll;

    // compute required width based on longest line
    int maxlen = 0; for (int i = 0; i < n; i++) { int l = (int)strlen(lines[i]); if (l > maxlen) maxlen = l; }
    int boxw = maxlen + 6; if (boxw > E.screencols - 4) boxw = E.screencols - 4; if (boxw < 40) boxw = 40;
    int boxh = content_h + 2;
    int sx = (E.screencols - boxw) / 2;
    int sy = (E.screenrows - boxh) / 2;

    // draw box background (clear whole region)
    for (int y = 0; y < boxh; y++) {
        char buf[128]; int ln = sy + y + 1; int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln, sx + 1);
        if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
        for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, " ", 1);
    }

    // draw text lines and pad to width to avoid artifacts; color section headers (ending with ':')
    for (int i = 0; i < content_h; i++) {
        int line_idx = E.help_scroll + i;
        if (line_idx >= n) break;
        int ln = sy + 1 + i + 1;
        int col = sx + 3;
        char buf[128]; int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln, col);
        if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
        const char *line = lines[line_idx]; int len = (int)strlen(line);
        int avail = boxw - (col - sx) - 2; if (avail < 0) avail = 0;
        int towrite = (len > avail) ? avail : len;
        // if this line looks like a section header (ends with ':' and not empty), color it cyan
        int is_header = (len > 0 && line[len-1] == ':');
        if (is_header) write(STDOUT_FILENO, "\x1b[96m", 5);
        if (towrite > 0) write(STDOUT_FILENO, line, (size_t)towrite);
        if (is_header) write(STDOUT_FILENO, "\x1b[0m", 4);
        // pad remainder of the line
        int pad = avail - towrite; for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1);
    }
}

static int visible_len(const char *s) {
    int v = 0;
    for (const char *p = s; *p; ) {
        if (*p == '\x1b' && p[1] == '[') {
            p += 2; while (*p && ((*p >= '0' && *p <= '9') || *p == ';')) p++; if (*p == 'm') p++; continue;
        }
        v++; p++;
    }
    return v;
}

static void draw_welcome_overlay(void) {
    // centered minimal welcome page with clearer color emphasis
    const char *lines[] = {
        "\x1b[1;96mWelcome to JED\x1b[0m",                   // bright/cyan title
        "\x1b[1;94mJamal Enhanced Editor\x1b[0m",         // bright/blue subtitle
        "",
        "\x1b[37mA small, fast terminal modal editor.\x1b[0m",
        "",
        "Type \x1b[1;33m'help'\x1b[0m for helpful tips.", // keys in yellow
        "Press \x1b[1;32mESC\x1b[0m to begin editing.", // start key in green
        "",
        "\x1b[93mTips:\x1b[0m",
        "Type \x1b[1;33m'b'\x1b[0m to open the file browser.",
        "Type \x1b[1;33m'open <filename>'\x1b[0m to open a file \x1b[1;33m(creates new if it doesn't exist)\x1b[0m.",
    };
    int n = sizeof(lines) / sizeof(lines[0]);
    // compute width based on visible lengths (ignore ANSI sequences)
    int maxvis = 0; for (int i = 0; i < n; i++) { int vl = visible_len(lines[i]); if (vl > maxvis) maxvis = vl; }
    int boxw = maxvis + 8; if (boxw > E.screencols - 4) boxw = E.screencols - 4; if (boxw < 40) boxw = 40;

    int sy = (E.screenrows / 2) - (n / 2);
    if (sy < 1) sy = 1;

    // Draw a subtle top and bottom divider to frame the content
    int pad_left = (E.screencols - boxw) / 2; if (pad_left < 0) pad_left = 0;
    char divbuf[256]; int dlen = snprintf(divbuf, sizeof(divbuf), "\x1b[%d;%dH", sy - 1, pad_left + 1);
    if (dlen > 0) write(STDOUT_FILENO, divbuf, (size_t)dlen);
    // draw top divider
    for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, "-", 1);

    for (int i = 0; i < n; i++) {
        const char *line = lines[i];
        int vl = visible_len(line);
        int col = (E.screencols - vl) / 2; if (col < 1) col = 1;
        char buf[128]; int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", sy + i, col);
        if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
        write(STDOUT_FILENO, line, strlen(line));
        // clear remainder of line to avoid artifacts
        int pad = (E.screencols - (col + vl - 1)); if (pad > 0) { for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1); }
    }

    // bottom divider
    int bd = snprintf(divbuf, sizeof(divbuf), "\x1b[%d;%dH", sy + n, pad_left + 1);
    if (bd > 0) write(STDOUT_FILENO, divbuf, (size_t)bd);
    for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, "-", 1);

    // Draw command line at bottom
    int cmd_row = E.screenrows;
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[K\x1b[1;37m:", cmd_row);
    write(STDOUT_FILENO, buf, strlen(buf));
    if (E.command_len > 0) write(STDOUT_FILENO, E.command_buf, E.command_len);
    write(STDOUT_FILENO, "\x1b[0m", 4); // reset
    // Position cursor
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cmd_row, 2 + E.command_len);
    write(STDOUT_FILENO, buf, strlen(buf));
}


// Helper: case-insensitive directory-first sort for entries
static int alphasort_entries(const void *a, const void *b) {
    const char **pa = (const char **)a;
    const char **pb = (const char **)b;
    const char *A = *pa;
    const char *B = *pb;
    // put directories (ending with '/') before files
    int da = (A[strlen(A)-1] == '/');
    int db = (B[strlen(B)-1] == '/');
    if (da != db) return db - da; // directories first
    // case-insensitive compare
    return strcasecmp(A, B);
}

// --- Filesystem fuzzy matching helpers -------------------------------------------------
// Return array of directory entries (names with trailing '/' for directories).
// Caller must free returned array and strings.
static char **dir_entries(const char *dir, int *out_n) {
    char **entries = NULL; int nentries = 0;
    DIR *d = opendir(dir ? dir : ".");
    if (!d) { if (out_n) *out_n = 0; return NULL; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0) continue; // skip self
        char *name = NULL;
        struct stat st;
        if (stat(de->d_name, &st) == 0 && S_ISDIR(st.st_mode)) {
            int l = (int)strlen(de->d_name) + 2; name = malloc(l); if (!name) continue; snprintf(name, l, "%s/", de->d_name);
        } else {
            name = strdup(de->d_name);
        }
        if (!name) continue;
        char **n = realloc(entries, sizeof(char*) * (nentries + 1)); if (!n) { free(name); continue; }
        entries = n; entries[nentries++] = name;
    }
    closedir(d);
    if (nentries == 0) { if (out_n) *out_n = 0; return entries; }
    qsort(entries, nentries, sizeof(char*), alphasort_entries);
    if (out_n) *out_n = nentries; return entries;
}

// Recursively collect directory entries (relative paths with trailing '/' for directories)
// Caller must free returned array and strings.
static char **recursive_dir_entries(const char *dir, int *out_n) {
    char **entries = NULL; int nentries = 0;
    const char *base = dir ? dir : ".";
    // recursive collector
    void collect(const char *curpath) {
        DIR *d = opendir(curpath);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            int plen = (int)strlen(curpath);
            int namelen = (int)strlen(de->d_name);
            int total = plen + 1 + namelen + 2; // include '/' and possible trailing '/' and NUL
            char *name = malloc(total);
            if (!name) continue;
            if (strcmp(curpath, ".") == 0) snprintf(name, total, "%s", de->d_name);
            else snprintf(name, total, "%s/%s", curpath, de->d_name);
            struct stat st;
            if (stat(name, &st) == 0 && S_ISDIR(st.st_mode)) {
                // append trailing '/'
                int l = (int)strlen(name);
                char *nname = realloc(name, l + 2);
                if (!nname) { free(name); continue; }
                name = nname; name[l] = '/'; name[l+1] = '\0';
                char **n = realloc(entries, sizeof(char*) * (nentries + 1)); if (!n) { free(name); continue; }
                entries = n; entries[nentries++] = name;
                // recurse
                char *subpath = NULL;
                if (strcmp(curpath, ".") == 0) subpath = strdup(de->d_name);
                else { subpath = malloc(plen + 1 + namelen + 1); if (subpath) snprintf(subpath, plen + 1 + namelen + 1, "%s/%s", curpath, de->d_name); }
                if (subpath) { collect(subpath); free(subpath); }
            } else {
                char **n = realloc(entries, sizeof(char*) * (nentries + 1)); if (!n) { free(name); continue; }
                entries = n; entries[nentries++] = name;
            }
        }
        closedir(d);
    }
    collect(base);
    if (nentries == 0) { if (out_n) *out_n = 0; return entries; }
    qsort(entries, nentries, sizeof(char*), alphasort_entries);
    if (out_n) *out_n = nentries; return entries;
}

// Simple fuzzy subsequence scorer: return -1 if not a subsequence, else higher is better.
// Scoring heuristics: longer matched runs and smaller gaps give higher score; matches at start or after '/','_','-' are rewarded.
static int fuzzy_score(const char *name, const char *pat) {
    if (!pat || !*pat) return 0;
    int n = (int)strlen(name); int m = (int)strlen(pat);
    int i = 0, j = 0;
    int score = 0; int last = -1; int consec = 0;
    while (i < n && j < m) {
        char cn = name[i]; char cp = pat[j];
        if (tolower((unsigned char)cn) == tolower((unsigned char)cp)) {
            int idx = i;
            int gap = (last == -1) ? idx : (idx - last - 1);
            // base points for matching a char
            int pts = 10;
            // reward consecutive matches
            if (last != -1 && idx == last + 1) { consec++; pts += 5 * consec; }
            else consec = 0;
            // reward if match at start or after separator
            if (idx == 0 || name[idx-1] == '/' || name[idx-1] == '_' || name[idx-1] == '-') pts += 8;
            // penalize gaps
            pts -= gap;
            score += pts;
            last = idx; j++; i++;
        } else {
            i++;
        }
    }
    if (j < m) return -1; // not a subsequence
    // Slight boost for longer patterns matched
    score += m * 2;
    return score;
}

// Comparator for match sorting: higher score first, then directory-first, then case-insensitive name
struct match_item { char *name; int score; };
static int match_cmp(const void *a, const void *b) {
    const struct match_item *A = (const struct match_item *)a;
    const struct match_item *B = (const struct match_item *)b;
    if (A->score != B->score) return B->score - A->score;
    int da = (A->name[strlen(A->name)-1] == '/');
    int db = (B->name[strlen(B->name)-1] == '/');
    if (da != db) return db - da;
    return strcasecmp(A->name, B->name);
}

// Return up to max matches (allocated array of strdup'd names); out_n is number returned. Caller must free.
static char **collect_dir_paths(const char *base, int max_depth, int *out_n) {
    char **list = NULL; int count = 0;
    const int MAX_DIR_COLLECT = 10000; // safety cap to avoid excessive memory/use

    void add_dir(const char *path) {
        if (count >= MAX_DIR_COLLECT) return;
        char **n = realloc(list, sizeof(char*) * (count + 1));
        if (!n) return;
        list = n;
        list[count] = strdup(path);
        if (list[count]) count++;
    }

    void recurse(const char *current, int depth) {
        if (depth >= max_depth) return;
        if (count >= MAX_DIR_COLLECT) return;
        DIR *d = opendir(current);
        if (!d) return;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (count >= MAX_DIR_COLLECT) break;
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
            char full[1024];
            if (snprintf(full, sizeof(full), "%s/%s", current, de->d_name) >= (int)sizeof(full)) continue;
            struct stat st;
            if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) {
                add_dir(full);
                recurse(full, depth + 1);
            }
        }
        closedir(d);
    }

    recurse(base, 0);
    *out_n = count;
    return list;
}

static char *normalize_display_path(const char *p) {
    if (!p) return NULL;
    if (p[0] == '.' && p[1] == '/') return strdup(p + 2);
    return strdup(p);
}

static char **get_fuzzy_matches(const char *dir, const char *pat, int max, int *out_n, int recursive, int executable_only, int search_directories) {
    if (search_directories) {
        // Try with max depth 6 first
        int ndirs = 0; char **dirs = collect_dir_paths(dir, 6, &ndirs);
        struct match_item *matches = malloc(sizeof(struct match_item) * ndirs);
        int mc = 0;
        for (int i = 0; i < ndirs; i++) {
            int sc = -1;
            if (!pat || !*pat) {
                sc = 0; /* empty pattern matches everything */
            } else {
                const char *cmp;
                if (strchr(pat, '/')) cmp = dirs[i];
                else { const char *p = strrchr(dirs[i], '/'); cmp = p ? p+1 : dirs[i]; }
                if (strncasecmp(cmp, pat, strlen(pat)) == 0) {
                    sc = 10000 - (int)strlen(dirs[i]);
                }
            }
            if (sc >= 0) {
                matches[mc].name = dirs[i]; matches[mc].score = sc; mc++;
                dirs[i] = NULL;
            }
        }
        for (int i = 0; i < ndirs; i++) if (dirs[i]) free(dirs[i]); free(dirs);
        if (mc == 0) {
            // No matches at depth 6, try depth 10
            ndirs = 0; dirs = collect_dir_paths(dir, 10, &ndirs);
            matches = realloc(matches, sizeof(struct match_item) * ndirs);
            mc = 0;
            for (int i = 0; i < ndirs; i++) {
                int sc = -1;
                if (!pat || !*pat) {
                    sc = 0;
                } else {
                    const char *cmp;
                    if (strchr(pat, '/')) cmp = dirs[i];
                    else { const char *p = strrchr(dirs[i], '/'); cmp = p ? p+1 : dirs[i]; }
                    if (strncasecmp(cmp, pat, strlen(pat)) == 0) {
                        sc = 10000 - (int)strlen(dirs[i]);
                    }
                }
                if (sc >= 0) {
                    matches[mc].name = dirs[i]; matches[mc].score = sc; mc++;
                    dirs[i] = NULL;
                }
            }
            for (int i = 0; i < ndirs; i++) if (dirs[i]) free(dirs[i]); free(dirs);
        }
        if (mc == 0) { free(matches); if (out_n) *out_n = 0; return NULL; }
        qsort(matches, mc, sizeof(struct match_item), match_cmp);
        int ret_n = (mc < max) ? mc : max;
        char **ret = malloc(sizeof(char*) * ret_n);
        for (int i = 0; i < ret_n; i++) ret[i] = normalize_display_path(matches[i].name);
        for (int i = 0; i < mc; i++) free(matches[i].name);
        free(matches);
        if (out_n) *out_n = ret_n; return ret;
    } else {
        int n = 0; char **entries = recursive ? recursive_dir_entries(dir, &n) : dir_entries(dir, &n);
        if (!entries || n == 0) { if (out_n) *out_n = 0; if (entries) free(entries); return NULL; }
        struct match_item *matches = malloc(sizeof(struct match_item) * n);
        int mc = 0;
        for (int i = 0; i < n; i++) {
            if (executable_only) {
                char fullpath[1024];
                if (snprintf(fullpath, sizeof(fullpath), "%s/%s", dir, entries[i]) >= (int)sizeof(fullpath)) continue;
                if (access(fullpath, X_OK) != 0) continue;
            }
            int sc = fuzzy_score(entries[i], pat);
            if (sc >= 0) {
                matches[mc].name = entries[i]; matches[mc].score = sc; mc++;
                entries[i] = NULL;
            }
        }
        for (int i = 0; i < n; i++) if (entries[i]) free(entries[i]); free(entries);
        if (mc == 0) { free(matches); if (out_n) *out_n = 0; return NULL; }
        qsort(matches, mc, sizeof(struct match_item), match_cmp);
        int ret_n = (mc < max) ? mc : max;
        char **ret = malloc(sizeof(char*) * ret_n);
        for (int i = 0; i < ret_n; i++) ret[i] = normalize_display_path(matches[i].name);
        for (int i = 0; i < mc; i++) free(matches[i].name);
        free(matches);
        if (out_n) *out_n = ret_n; return ret;
    }
}

static void free_matches(char **m, int n) { if (!m) return; for (int i = 0; i < n; i++) free(m[i]); free(m); }



/* Search backend: run ripgrep (rg) if available, otherwise grep. Returns
 * an array of strdup'd strings formatted as "path:lineno: snippet" up to
 * `max` results. If the query is quoted ("...") a fixed-string exact
 * match is used. Caller must free returned array via free_matches(). */
static char **run_search(const char *query, int max, int *out_n) {
    if (!query || !*query) { if (out_n) *out_n = 0; return NULL; }
    // detect quoted exact match
    int exact = 0; char qcopy[512]; size_t qlen = strlen(query);
    if (qlen >= 2 && query[0] == '"' && query[qlen-1] == '"') {
        exact = 1; size_t copylen = qlen - 2; if (copylen >= sizeof(qcopy)) copylen = sizeof(qcopy)-1; memcpy(qcopy, &query[1], copylen); qcopy[copylen] = '\0';
    } else {
        size_t copylen = qlen; if (copylen >= sizeof(qcopy)) copylen = sizeof(qcopy)-1; memcpy(qcopy, query, copylen); qcopy[copylen] = '\0';
    }

    int pfd[2]; if (pipe(pfd) == -1) { if (out_n) *out_n = 0; return NULL; }
    pid_t pid = fork();
    if (pid == -1) { close(pfd[0]); close(pfd[1]); if (out_n) *out_n = 0; return NULL; }
    if (pid == 0) {
        close(pfd[0]); if (dup2(pfd[1], STDOUT_FILENO) == -1) _exit(127); close(pfd[1]);
        if (exact) {
            char pattern[512];
            snprintf(pattern, sizeof(pattern), "\\<%s", qcopy);
            char *argv[] = {"rg", "--no-heading", "--line-number", "--hidden", "--glob", "!.git", "-i", "-e", pattern, ".", NULL};
            execvp("rg", argv);
            char *argv2[] = {"grep", "-RInE", "--exclude-dir=.git", pattern, ".", NULL};
            execvp("grep", argv2);
        } else {
            char *argv[] = {"rg", "--no-heading", "--line-number", "--hidden", "--glob", "!.git", "-S", qcopy, ".", NULL};
            execvp("rg", argv);
            char *argv2[] = {"grep", "-RIn", "--exclude-dir=.git", qcopy, ".", NULL};
            execvp("grep", argv2);
        }
        _exit(127);
    }

    close(pfd[1]); FILE *f = fdopen(pfd[0], "r"); if (!f) { close(pfd[0]); waitpid(pid, NULL, 0); if (out_n) *out_n = 0; return NULL; }
    int capacity = (max > 0) ? (max + 1) : 64; char **results = malloc(sizeof(char*) * capacity); int n = 0;
    char buf[1024]; while (fgets(buf, sizeof(buf), f)) {
        if (n >= max) break;
        // trim newline
        size_t L = strlen(buf); while (L > 0 && (buf[L-1] == '\n' || buf[L-1] == '\r')) buf[--L] = '\0';
        // parse path:lineno:rest
        const char *p1 = strchr(buf, ':'); if (!p1) continue;
        size_t pathlen = p1 - buf; const char *p2 = strchr(p1 + 1, ':'); if (!p2) continue;
        char path[512]; size_t plen = (pathlen < sizeof(path)-1) ? pathlen : (sizeof(path)-1); memcpy(path, buf, plen); path[plen] = '\0';
        char linenobuf[32]; size_t llen = p2 - (p1 + 1); if (llen >= sizeof(linenobuf)) llen = sizeof(linenobuf)-1; memcpy(linenobuf, p1+1, llen); linenobuf[llen] = '\0';
        int lineno = atoi(linenobuf);
        const char *rest = p2 + 1;
        // find match in rest and highlight
        char snippet[512]; const char *matchloc = NULL;
        // case-insensitive search for the query
        const char *r = rest; size_t ql = strlen(qcopy);
        while (*r) {
            size_t i; for (i = 0; i < ql; i++) {
                char a = r[i]; char b = qcopy[i]; if (!a) { i = ql; break; }
                if (tolower((unsigned char)a) != tolower((unsigned char)b)) break;
            }
            if (i == ql) { matchloc = r; break; }
            r++;
        }
        if (matchloc) {
            /* ensure match is at word-start when exact quoted query is used */
            if (exact) {
                const char *search = rest; const char *ml = NULL;
                while ((ml = strstr(search, qcopy)) != NULL) {
                    int before = (int)(ml - rest);
                    if (before == 0 || !(isalnum((unsigned char)rest[before-1]) || rest[before-1] == '_')) { matchloc = ml; break; }
                    search = ml + 1; /* continue searching for next occurrence */
                }
                if (!matchloc) { /* no suitable word-start match */; }
            }
            if (!matchloc) continue;
            // build context around match (50 chars each side)
            int before = (int)(matchloc - rest);
            int start = before - 50; if (start < 0) start = 0;
            int matchlen = strlen(qcopy);
            int restlen = (int)strlen(rest);
            int end = before + matchlen + 50; if (end > restlen) end = restlen;
            int outlen = end - start; if (outlen >= (int)sizeof(snippet)-1) outlen = (int)sizeof(snippet)-5;
            if (start > 0) { snprintf(snippet, sizeof(snippet), "...%.*s", outlen, rest + start); } else { snprintf(snippet, sizeof(snippet), "%.*s", outlen, rest + start); }
            // now find position within snippet to highlight
            int pos = 0; int found = 0;
            const char *rr = snippet; size_t ql = strlen(qcopy);
            while (*rr) {
                size_t i; for (i = 0; i < ql; i++) {
                    char a = rr[i]; char b = qcopy[i]; if (!a) { i = ql; break; }
                    if (tolower((unsigned char)a) != tolower((unsigned char)b)) break;
                }
                if (i == ql) { pos = rr - snippet; found = 1; break; }
                rr++;
            }
            if (found) {
                char out[700]; snprintf(out, sizeof(out), "%.*s\x1b[1;30;42m%.*s\x1b[0m%.*s",
                    pos, snippet,
                    (int)strlen(qcopy), snippet + pos,
                    (int)(strlen(snippet) - pos - strlen(qcopy)), snippet + pos + strlen(qcopy));
                char full[2048];
                const char *display_path = path;
                if (display_path[0] == '.' && display_path[1] == '/') display_path += 2;
                snprintf(full, sizeof(full), "%s:%d: %s", display_path, lineno, out);
                /* Sanitize snippet by removing control characters (non-printable except common whitespace) */
                for (char *p = full; *p; p++) {
                    unsigned char uc = (unsigned char)*p;
                    if (uc != '\t' && uc != '\n' && uc != '\r' && (uc < 32 || uc == 127)) {
                        /* replace with space to avoid rendering/terminal issues */
                        *p = ' ';
                    }
                }
                /* include matches from all files (do not skip project source files) */
                results[n++] = strdup(full);
                continue;
            }
        }
        // fallback: include trimmed rest
        char out[700]; if (L > 600) { buf[597] = '.'; buf[598] = '.'; buf[599] = '.'; buf[600] = '\0'; }
        snprintf(out, sizeof(out), "%s", buf);
        /* sanitize fallback */
        for (char *p = out; *p; p++) { unsigned char uc = (unsigned char)*p; if (uc != '\t' && uc != '\n' && uc != '\r' && (uc < 32 || uc == 127)) *p = ' '; }
        results[n++] = strdup(out);
    }
    fclose(f); waitpid(pid, NULL, 0);
    if (n == 0) { free(results); if (out_n) *out_n = 0; return NULL; }
    if (out_n) *out_n = n; return results;
}

/* Open file and jump to the given 1-based line number (if valid). */
static void open_file_and_seek(const char *fname, int lineno) {
    if (!fname) return;
    FILE *f = fopen(fname, "r"); if (!f) { snprintf(E.msg, sizeof(E.msg), "Cannot open %s", fname); E.msg_time = time(NULL); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, sz, f); buf[sz] = '\0'; fclose(f);
    load_buffer_from_string(buf); free(buf);
    free(E.filename); E.filename = strdup(fname);
    save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
    set_language_from_filename(fname);
    /* set cursor to line */
    if (lineno < 1) lineno = 1; if (lineno > E.numrows) lineno = E.numrows;
    E.cy = lineno - 1; if (E.cy < 0) E.cy = 0; E.cx = 0; editorScroll();
    snprintf(E.msg, sizeof(E.msg), "Opened %s", fname); E.msg_time = time(NULL); E.welcome_visible = 0;
}

// Active suggestion state for live completion
static char **ActiveMatches = NULL;
static int ActiveMatchesN = 0;
static int ActiveMatchSel = 0;
static int ActiveMatchesPage = 0; /* pagination */
static int ActiveFilenameStart = -1; // offset in E.command_buf where filename begins
static int SuggestionsVisible = 0;
static int SuggestionMax = 8; // default suggestion limit (within 5-10 per design)
static int SuggestionPageSize = 8; // default page size for suggestion overlay
static const char *CurrentSearchDir = NULL; // directory used for the most recent suggestion search (e.g. ".")

static void clear_active_suggestions(void) {
    free_matches(ActiveMatches, ActiveMatchesN);
    ActiveMatches = NULL; ActiveMatchesN = 0; ActiveMatchSel = 0; ActiveFilenameStart = -1; SuggestionsVisible = 0;
}

// Update suggestions based on current E.command_buf. Shows suggestions if command is 'open' or 'opent'.
static void update_suggestions_from_command(void) {
    // clear previous
    clear_active_suggestions();
    const char *cmds[] = {"open", "opent"};
    for (int ci = 0; ci < (int)(sizeof(cmds)/sizeof(cmds[0])); ci++) {
        const char *kw = cmds[ci]; size_t kwlen = strlen(kw);
        if (E.command_len >= (int)kwlen && strncmp(E.command_buf, kw, kwlen) == 0) {
            if (E.command_len == (int)kwlen || E.command_buf[kwlen] == ' ') {
                int pos = (int)kwlen; while (pos < E.command_len && E.command_buf[pos] == ' ') pos++;
                // pattern is remainder (may be empty)
                char pat[512] = {0}; int plen = 0;
                if (pos < E.command_len) { plen = E.command_len - pos; if (plen > (int)sizeof(pat)-1) plen = (int)sizeof(pat)-1; memcpy(pat, &E.command_buf[pos], plen); pat[plen] = '\0'; }
                int n = 0;
                char **matches;
                if (pat[0] == '/') {
                    // directory search from root
                    char *new_pat = pat + 1;
                    matches = get_fuzzy_matches("/", new_pat, SuggestionMax, &n, 1, 0, 1);
                } else {
                    matches = get_fuzzy_matches(".", pat, SuggestionMax, &n, 0, 0, 0);
                }
                if (n > 0) {
                    ActiveMatches = matches; ActiveMatchesN = n; ActiveMatchSel = 0; ActiveFilenameStart = pos; SuggestionsVisible = 1; return;
                } else {
                    // no matches: keep suggestions hidden
                    clear_active_suggestions(); return;
                }
            }
        }
    }
    // support `find` command: show search matches
    const char *fkw = "find"; size_t fkwlen = strlen(fkw);
    if (E.command_len >= (int)fkwlen && strncmp(E.command_buf, fkw, fkwlen) == 0) {
        if (E.command_len > (int)fkwlen && E.command_buf[fkwlen] == ' ') {
            int pos = (int)fkwlen; while (pos < E.command_len && E.command_buf[pos] == ' ') pos++;
            char pat[512] = {0}; int plen = 0;
            if (pos < E.command_len) { plen = E.command_len - pos; if (plen > (int)sizeof(pat)-1) plen = (int)sizeof(pat)-1; memcpy(pat, &E.command_buf[pos], plen); pat[plen] = '\0'; }
            int n = 0;
            // allow more results for search
            char **matches = run_search(pat, 50, &n);
            if (n > 0) {
                ActiveMatches = matches; ActiveMatchesN = n; ActiveMatchSel = 0; ActiveFilenameStart = -1; SuggestionsVisible = 1; return;
            } else { clear_active_suggestions(); return; }
        }
    }

    // support `run` command: show executable matches
    const char *rkw = "run"; size_t rkwlen = strlen(rkw);
    if (E.command_len >= (int)rkwlen && strncmp(E.command_buf, rkw, rkwlen) == 0) {
        if (E.command_len == (int)rkwlen || E.command_buf[rkwlen] == ' ') {
            int pos = (int)rkwlen; while (pos < E.command_len && E.command_buf[pos] == ' ') pos++;
            char pat[512] = {0}; int plen = 0;
            if (pos < E.command_len) { plen = E.command_len - pos; if (plen > (int)sizeof(pat)-1) plen = (int)sizeof(pat)-1; memcpy(pat, &E.command_buf[pos], plen); pat[plen] = '\0'; }
            CurrentSearchDir = ".";
            int n = 0;
            char **matches = get_fuzzy_matches(".", pat, SuggestionMax, &n, 0, 1, 0);
            if (n > 0) {
                ActiveMatches = matches; ActiveMatchesN = n; ActiveMatchSel = 0; ActiveFilenameStart = pos; SuggestionsVisible = 1; return;
            } else {
                clear_active_suggestions(); return;
            }
        }
    }
    // support `R` command: same as run (single-letter uppercase alias)
    const char *rrkw = "R"; size_t rrkwlen = strlen(rrkw);
    if (E.command_len >= (int)rrkwlen && strncmp(E.command_buf, rrkw, rrkwlen) == 0) {
        if (E.command_len == (int)rrkwlen || E.command_buf[rrkwlen] == ' ') {
            int pos = (int)rrkwlen; while (pos < E.command_len && E.command_buf[pos] == ' ') pos++;
            char pat[512] = {0}; int plen = 0;
            if (pos < E.command_len) { plen = E.command_len - pos; if (plen > (int)sizeof(pat)-1) plen = (int)sizeof(pat)-1; memcpy(pat, &E.command_buf[pos], plen); pat[plen] = '\0'; }
            CurrentSearchDir = ".";
            int n = 0;
            char **matches = get_fuzzy_matches(".", pat, SuggestionMax, &n, 0, 1, 0);
            if (n > 0) {
                ActiveMatches = matches; ActiveMatchesN = n; ActiveMatchSel = 0; ActiveFilenameStart = pos; SuggestionsVisible = 1; return;
            } else {
                clear_active_suggestions(); return;
            }
        }
    }

    // not an open/opent/find/run command: ensure hidden
    clear_active_suggestions();
}

// render suggestions overlay (non-blocking; drawn from refresh)
static void render_suggestions_overlay(void) {
    if (!SuggestionsVisible || ActiveMatchesN <= 0) return;
    int n = ActiveMatchesN;
    int sel = ActiveMatchSel;
    if (sel < 0) sel = 0; if (sel >= n) sel = n - 1;
    int maxlen = 0; for (int i = 0; i < n; i++) { int l = visible_len(ActiveMatches[i]); if (l > maxlen) maxlen = l; }
    int full_page = (ActiveFilenameStart < 0); // full page for search
    int boxw, boxh, sx, sy;
    if (full_page) {
        boxw = E.screencols;
        boxh = E.screenrows;
        sx = 0; sy = 0;
    } else {
        boxw = maxlen + 6; if (boxw > E.screencols) boxw = E.screencols;
        boxh = n + 4; if (boxh > E.screenrows) boxh = E.screenrows;
        sx = (E.screencols - boxw) / 2; sy = (E.screenrows - boxh) / 2;
    }
    // clear box area
    if (full_page) {
        write(STDOUT_FILENO, "\x1b[2J", 4); // clear entire screen
        write(STDOUT_FILENO, "\x1b[H", 3);  // move to top-left
    } else {
        for (int y = 0; y < boxh; y++) {
            char buf[128]; int ln = sy + y + 1;
            int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln, sx + 1);
            if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
            for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, " ", 1);
        }
    }
    // title
    char title[2048];
    if (ActiveFilenameStart < 0 && E.command_len > 0) {
        // search: include command in white
        snprintf(title, sizeof(title), "Suggestions (%d) : \x1b[37m%s\x1b[0m", n, E.command_buf);
    } else {
        snprintf(title, sizeof(title), "Suggestions (%d)", n);
    }
    int col = full_page ? 1 : sx + 2; int ln = full_page ? 1 : sy + 1; char linebuf[512]; int pn = snprintf(linebuf, sizeof(linebuf), "\x1b[%d;%dH%s", ln, col, title); if (pn>0) write(STDOUT_FILENO, linebuf, (size_t)pn);
    // list entries (with pagination)
    int pageSize = full_page ? (E.screenrows - 4) : ((E.screenrows - 4 > 3) ? (E.screenrows - 4) : SuggestionPageSize); if (pageSize < 3) pageSize = 3;
    int pages = (n + pageSize - 1) / pageSize; if (ActiveMatchesPage >= pages) ActiveMatchesPage = pages ? pages - 1 : 0;
    int start = ActiveMatchesPage * pageSize; int end = start + pageSize; if (end > n) end = n;

    for (int idx = start, linei = 0; idx < end; idx++, linei++) {
        int ln2 = full_page ? (ln + 2 + linei) : (sy + 2 + linei);
        int col2 = full_page ? 1 : (sx + 2);
        char buf[512]; int rn = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln2, col2);
        if (rn>0) write(STDOUT_FILENO, buf, (size_t)rn);
        int isSel = (idx == sel);
        if (isSel) write(STDOUT_FILENO, "\x1b[7m", 4);
        /* If this is a search result (ActiveFilenameStart < 0) the entry is
           formatted as "path:lineno: snippet"; print path in green then snippet
           (which may contain ANSI highlighting already). */
        if (ActiveFilenameStart < 0) {
            const char *ent = ActiveMatches[idx]; const char *c1 = strchr(ent, ':');
            if (c1) {
                size_t plen = c1 - ent; write(STDOUT_FILENO, "\x1b[32m", 5); write(STDOUT_FILENO, ent, plen); write(STDOUT_FILENO, "\x1b[0m", 4);
                // write remainder including ':' and snippet
                write(STDOUT_FILENO, c1, strlen(c1));
            } else {
                write(STDOUT_FILENO, ActiveMatches[idx], strlen(ActiveMatches[idx]));
            }
        } else {
            int is_dir = (ActiveMatches[idx][strlen(ActiveMatches[idx]) - 1] == '/');
            if (is_dir) write(STDOUT_FILENO, "\x1b[32m", 5);
            else write(STDOUT_FILENO, "\x1b[32m", 5); /* make non-dir green instead of yellow */
            write(STDOUT_FILENO, ActiveMatches[idx], strlen(ActiveMatches[idx]));
            write(STDOUT_FILENO, "\x1b[0m", 4);
        }
        int pad = boxw - 4 - visible_len(ActiveMatches[idx]); if (pad > 0) { for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1); }
        if (isSel) write(STDOUT_FILENO, "\x1b[m", 3);
    }
    // footer showing page info
    int foot_ln = full_page ? (E.screenrows) : (sy + boxh - 1);
    int foot_col = full_page ? 1 : (sx + 2);
    char foot[128]; int fp = snprintf(foot, sizeof(foot), "\x1b[%d;%dHPage %d/%d (%d results)", foot_ln, foot_col, ActiveMatchesPage + 1, pages ? pages : 1, n);
    if (fp > 0) write(STDOUT_FILENO, foot, (size_t)fp);
    // position cursor and show it
    write(STDOUT_FILENO, "\x1b[?25h", 6); // show cursor
    if (E.mode == MODE_VIEW && ActiveFilenameStart < 0) {
        // position cursor at end of visible title (account for ANSI codes)
        int cursor_col = col + visible_len(title);
        if (cursor_col < 1) cursor_col = 1;
        if (cursor_col > E.screencols) cursor_col = E.screencols;
        char cur[64]; int m = snprintf(cur, sizeof(cur), "\x1b[%d;%dH", ln, cursor_col);
        if (m > 0) write(STDOUT_FILENO, cur, (size_t)m);
    } else {
        // position cursor out of way
        int out_ln = full_page ? E.screenrows : (sy + boxh);
        int out_col = full_page ? E.screencols : (sx + 1);
        char cur[64]; int m = snprintf(cur, sizeof(cur), "\x1b[%d;%dH", out_ln, out_col);
        if (m > 0) write(STDOUT_FILENO, cur, (size_t)m);
    }
}

// accept current active suggestion and insert into command buffer (does not execute command)
static void accept_active_suggestion(void) {
    if (!SuggestionsVisible || ActiveMatchesN <= 0) return;
    int sel = ActiveMatchSel; if (sel < 0 || sel >= ActiveMatchesN) sel = 0;
    if (ActiveFilenameStart >= 0) {
        /* filename completion into command buffer */
        int addlen = (int)strlen(ActiveMatches[sel]);
        int pre = ActiveFilenameStart;
        int newlen = pre + addlen;
        /* detect if this match is a directory; build full path if necessary */
        char selpath[1024]; selpath[0] = '\0';
        const char *mname = ActiveMatches[sel];
        if (!mname) { clear_active_suggestions(); return; }
        if (mname[0] == '/') {
            strncpy(selpath, mname, sizeof(selpath)-1);
        } else {
            /* use CurrentSearchDir if set, otherwise treat as relative */
            const char *base = CurrentSearchDir && *CurrentSearchDir ? CurrentSearchDir : ".";
            if (snprintf(selpath, sizeof(selpath), "%s/%s", base, mname) >= (int)sizeof(selpath)) selpath[0] = '\0';
        }
        struct stat st; int is_dir = 0;
        if (selpath[0] && stat(selpath, &st) == 0 && S_ISDIR(st.st_mode)) is_dir = 1;
        if (is_dir) {
            /* show same directory prompt as file browser */
            char tmpn[512]; strncpy(tmpn, selpath, sizeof(tmpn)-1); tmpn[sizeof(tmpn)-1] = '\0';
            /* strip trailing slash for display if present */
            size_t tlen = strlen(tmpn); if (tlen > 0 && tmpn[tlen-1] == '/') tmpn[tlen-1] = '\0';
            char __tmpbuf[256]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
            if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
            int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "Directory '%s': \x1b[92m(o) Open\x1b[0m / \x1b[91m(d) Delete\x1b[0m / \x1b[94m(g) Change Directory\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tmpn);
            if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
            int dresp = 0;
            while (1) {
                int kk = readKey(); if (kk == -1) continue;
                if (kk == 'o' || kk == 'O' || kk == '\r' || kk == '\n') { dresp = 'o'; break; }
                if (kk == 'd' || kk == 'D') { dresp = 'd'; break; }
                if (kk == 'g' || kk == 'G') { dresp = 'g'; break; }
                if (kk == 'c' || kk == 'C' || kk == 27) { dresp = 'c'; break; }
            }
            __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
            if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
            clear_active_suggestions();
            /* apply choice rules */
            if (dresp == 'c') { snprintf(E.msg, sizeof(E.msg), "Cancelled"); E.msg_time = time(NULL); E.command_len = 0; E.command_buf[0] = '\0'; return; }
            if (dresp == 'o') {
                if (chdir(selpath) == 0) {
                    show_file_browser();
                } else {
                    snprintf(E.msg, sizeof(E.msg), "Chdir failed: %s", strerror(errno)); E.msg_time = time(NULL);
                }
                E.command_len = 0; E.command_buf[0] = '\0'; return;
            }
            if (dresp == 'g') {
                if (chdir(selpath) == 0) {
                    char newcwd[512]; getcwd(newcwd, sizeof(newcwd)); snprintf(E.msg, sizeof(E.msg), "Changed directory to %s", newcwd); E.msg_time = time(NULL);
                } else {
                    snprintf(E.msg, sizeof(E.msg), "Chdir failed: %s", strerror(errno)); E.msg_time = time(NULL);
                }
                E.command_len = 0; E.command_buf[0] = '\0'; return;
            }
            if (dresp == 'd') {
                /* confirm delete */
                char __tmp2[256]; int __n2 = snprintf(__tmp2, sizeof(__tmp2), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__n2 > 0) write(STDOUT_FILENO, __tmp2, (size_t)__n2);
                int pn2 = snprintf(__tmp2, sizeof(__tmp2), "\x1b[93mDelete directory '%s'?\x1b[0m \x1b[92m(y)\x1b[0m\x1b[91m(N)\x1b[0m: ", tmpn);
                if (pn2 > 0) write(STDOUT_FILENO, __tmp2, (size_t)pn2);
                int y = 0; while (1) { int k2 = readKey(); if (k2 == -1) continue; if (k2=='y'||k2=='Y') { y = 1; break; } if (k2=='n'||k2=='N'||k2==27) { y = 0; break; } }
                __n2 = snprintf(__tmp2, sizeof(__tmp2), "\x1b[%d;1H\x1b[K", E.screenrows); if (__n2>0) write(STDOUT_FILENO, __tmp2, (size_t)__n2);
                if (!y) { snprintf(E.msg, sizeof(E.msg), "Delete cancelled"); E.msg_time = time(NULL); E.command_len = 0; E.command_buf[0] = '\0'; return; }
                if (rmdir(selpath) == 0) snprintf(E.msg, sizeof(E.msg), "Deleted directory %s", tmpn); else snprintf(E.msg, sizeof(E.msg), "rmdir failed: %s", strerror(errno));
                E.msg_time = time(NULL); E.command_len = 0; E.command_buf[0] = '\0'; return;
            }
        }
        /* not a directory: insert into command buffer as before */
        if (newlen + 1 < (int)sizeof(E.command_buf)) {
            memcpy(&E.command_buf[pre], ActiveMatches[sel], addlen);
            E.command_len = newlen; E.command_buf[E.command_len] = '\0';
        }
        clear_active_suggestions();
        return;
    }
    /* if ActiveFilenameStart < 0, treat matches as search results of form "path:lineno: snippet" */
    const char *entry = ActiveMatches[sel];
    if (!entry) { clear_active_suggestions(); return; }
    /* parse path and lineno
       formats: path:lineno:rest  OR path:lineno:col:rest (handle first two parts) */
    const char *p = entry;
    const char *c1 = strchr(p, ':'); if (!c1) { clear_active_suggestions(); return; }
    size_t pathlen = c1 - p;
    char pathbuf[512]; if (pathlen >= sizeof(pathbuf)) pathlen = sizeof(pathbuf)-1;
    memcpy(pathbuf, p, pathlen); pathbuf[pathlen] = '\0';
    const char *p2 = c1 + 1; const char *c2 = strchr(p2, ':'); int lineno = 1;
    if (c2) {
        char numbuf[32]; size_t nlen = c2 - p2; if (nlen >= sizeof(numbuf)) nlen = sizeof(numbuf)-1;
        memcpy(numbuf, p2, nlen); numbuf[nlen] = '\0'; lineno = atoi(numbuf);
    }
    // check if file is already open in some tab or current
    int already_open = 0;
    if (E.filename && strcmp(E.filename, pathbuf) == 0) {
        already_open = 1;
    } else {
        for (int i = 0; i < NumTabs; i++) {
            if (Tabs[i].name && strcmp(Tabs[i].name, pathbuf) == 0) {
                already_open = 1;
                // switch to that tab
                if (CurTab != i) {
                    if (CurTab == -1) save_current_to_home();
                    CurTab = i;
                    load_tab_snapshot(i);
                }
                break;
            }
        }
    }
    if (already_open) {
        // seek to line
        editorSeekLine(lineno);
        clear_active_suggestions();
        // clear command buffer after opening
        E.command_len = 0;
        E.command_buf[0] = '\0';
    } else {
        // prompt to open
        clear_active_suggestions();
        E.prompt_visible = 1;
        snprintf(E.prompt_message, sizeof(E.prompt_message), "File '%s' is not open. Open it?", pathbuf);
        /* use themed bold colors and color the descriptive words too */
        snprintf(E.prompt_options, sizeof(E.prompt_options), "\x1b[1;92mo\x1b[0m: \x1b[92mcurrent tab\x1b[0m, \x1b[1;94mt\x1b[0m: \x1b[94mnew tab\x1b[0m, \x1b[1;93mc\x1b[0m: \x1b[93mcancel\x1b[0m");
        strcpy(E.pending_path, pathbuf);
        E.pending_lineno = lineno;
        // clear command buffer
        E.command_len = 0;
        E.command_buf[0] = '\0';
    }
}

// Blocking picker (used by Tab-triggered flow). Returns selected index or -1 if cancelled.
static int pick_from_matches(char **matches, int n) {
    if (!matches || n <= 0) return -1;
    int sel = 0;
    while (1) {
        if (winch_received) { getWindowSize(); editorRefreshScreen(); winch_received = 0; }
        // compute max visible length (account for ANSI sequences)
        int maxlen = 0; for (int i = 0; i < n; i++) { int l = visible_len(matches[i]); if (l > maxlen) maxlen = l; }
        int colw = maxlen + 6; if (colw < 18) colw = 18; // minimum column width
        int cols = E.screencols / colw; if (cols < 1) cols = 1; if (cols > n) cols = n;
        int rows = (n + cols - 1) / cols;
        int boxw = cols * colw; if (boxw > E.screencols) boxw = E.screencols;
        int boxh = rows + 4; if (boxh > E.screenrows) boxh = E.screenrows;
        int sx = (E.screencols - boxw) / 2; int sy = (E.screenrows - boxh) / 2;
        // clear the full screen to avoid overlay artifacts from previous popups
        write(STDOUT_FILENO, "\x1b[2J", 4); // clear entire screen
        write(STDOUT_FILENO, "\x1b[H", 3);    // move to top-left
        // draw box background (clear box region explicitly for clarity)
        for (int y = 0; y < boxh; y++) {
            char buf[128]; int ln = sy + y + 1;
            int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln, sx + 1);
            if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
            for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, " ", 1);
        }
        // title (bold white) + small hint (dim)
        char title[128]; snprintf(title, sizeof(title), "\x1b[1;97mSelect an option\x1b[0m \x1b[90m(use arrows, Enter to select, q to cancel)\x1b[0m");
        int col = sx + 2; int ln = sy + 1; char linebuf[256]; int pn = snprintf(linebuf, sizeof(linebuf), "\x1b[%d;%dH%s", ln, col, title); if (pn>0) write(STDOUT_FILENO, linebuf, (size_t)pn);
        // draw items in columns (column-major ordering for balanced display)
        for (int r = 0; r < rows; r++) {
            int ln2 = sy + 2 + r;
            for (int c = 0; c < cols; c++) {
                int idx = r + c * rows; if (idx >= n) continue;
                int colstart = sx + 2 + c * colw;
                char buf[1024]; int rn = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln2, colstart);
                if (rn > 0) write(STDOUT_FILENO, buf, (size_t)rn);
                int isSel = (idx == sel);
                if (isSel) write(STDOUT_FILENO, "\x1b[7m", 4); // reverse for selection
                // number (cyan) then item (yellow/green for dirs)
                char numbuf[32]; snprintf(numbuf, sizeof(numbuf), "\x1b[96m%2d)\x1b[0m ", idx+1);
                write(STDOUT_FILENO, numbuf, strlen(numbuf));
                int is_dir = (matches[idx][strlen(matches[idx]) - 1] == '/');
                if (is_dir) write(STDOUT_FILENO, "\x1b[92m", 5);
                else write(STDOUT_FILENO, "\x1b[37m", 5);
                write(STDOUT_FILENO, matches[idx], strlen(matches[idx]));
                write(STDOUT_FILENO, "\x1b[0m", 4);
                // pad to column width
                int pad = colw - 4 - visible_len(matches[idx]); if (pad > 0) { for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1); }
                if (isSel) write(STDOUT_FILENO, "\x1b[m", 3);
            }
        }
        // footer: show index hint
        int foot_ln = sy + boxh - 1; int foot_col = sx + 2;
        char foot[128]; int fp = snprintf(foot, sizeof(foot), "\x1b[%d;%dH\x1b[90mPress number to jump directly, arrows to move, Enter to pick\x1b[0m", foot_ln, foot_col);
        if (fp > 0) write(STDOUT_FILENO, foot, (size_t)fp);
        // position cursor out of way
        char cur[64]; int m = snprintf(cur, sizeof(cur), "\x1b[%d;%dH", sy + boxh, sx + 1); if (m>0) write(STDOUT_FILENO, cur, (size_t)m);
        // read key
        int k = readKey(); if (k == -1) continue;
        if (k >= '1' && k <= '9') {
            int v = k - '0'; if (v >= 1 && v <= n) return v - 1; continue;
        }
        if (k == ARROW_DOWN) {
            // move down by one row (wrap)
            int r = sel % rows; int c = sel / rows; r = (r + 1) % rows; int newsel = r + c * rows; if (newsel >= n) newsel = sel; sel = newsel; continue;
        }
        if (k == ARROW_UP) {
            int r = sel % rows; int c = sel / rows; r = (r - 1 + rows) % rows; int newsel = r + c * rows; if (newsel >= n) newsel = sel; sel = newsel; continue;
        }
        if (k == ARROW_LEFT) {
            int r = sel % rows; int c = sel / rows; c = (c - 1 + cols) % cols; int newsel = r + c * rows; if (newsel < n) sel = newsel; continue;
        }
        if (k == ARROW_RIGHT) {
            int r = sel % rows; int c = sel / rows; c = (c + 1) % cols; int newsel = r + c * rows; if (newsel < n) sel = newsel; continue;
        }
        if (k == '\r' || k == '\n' || k == '\t') { 
            // restore screen and refresh before returning
            write(STDOUT_FILENO, "\x1b[2J", 4); write(STDOUT_FILENO, "\x1b[H", 3); editorRefreshScreen();
            return sel; 
        }
        if (k == 27 || k == 'q' || k == 'Q') { return -1; }
    }
}

// Multi-select picker (implementation placed after single-select picker)
static int pick_multi_from_matches_with_desc(char **labels, char **descs, int n, int *out_flags) {
    if (!labels || n <= 0 || !out_flags) return -1;
    for (int i = 0; i < n; i++) out_flags[i] = 0;
    int sel = 0;
    while (1) {
        if (winch_received) { getWindowSize(); editorRefreshScreen(); winch_received = 0; }
        int maxlabel = 0; int maxdesc = 0; for (int i = 0; i < n; i++) { int lv = visible_len(labels[i]); if (lv > maxlabel) maxlabel = lv; if (descs && descs[i]) { int dv = visible_len(descs[i]); if (dv > maxdesc) maxdesc = dv; } }
        int colw = maxlabel + maxdesc + 12; if (colw < 40) colw = 40; if (colw > E.screencols - 4) colw = E.screencols - 4;
        int rows = (n < (E.screenrows - 6)) ? n : (E.screenrows - 6);
        int boxh = rows + 4; int boxw = colw; int sx = (E.screencols - boxw) / 2; int sy = (E.screenrows - boxh) / 2;
        // clear the full screen to avoid overlay artifacts from previous popups
        write(STDOUT_FILENO, "\x1b[2J", 4); // clear entire screen
        write(STDOUT_FILENO, "\x1b[H", 3);    // move to top-left
        // draw box background (clear box region explicitly for clarity)
        for (int y = 0; y < boxh; y++) {
            char buf[128]; int ln = sy + y + 1; int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln, sx + 1); if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
            for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, " ", 1);
        }
        // title
        char title[256]; snprintf(title, sizeof(title), "\x1b[1;97mSelect options (Space to toggle, Enter to accept)\x1b[0m"); int col = sx + 2; int ln = sy + 1; char linebuf[256]; int pn = snprintf(linebuf, sizeof(linebuf), "\x1b[%d;%dH%s", ln, col, title); if (pn>0) write(STDOUT_FILENO, linebuf, (size_t)pn);
        // list (simple scrolling)
        int start = 0; if (sel >= rows) start = sel - rows + 1; int end = start + rows; if (end > n) end = n;
        for (int i = start; i < end; i++) {
            int ln2 = sy + 2 + (i - start); int col2 = sx + 2; char buf[1024]; int rn = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln2, col2); if (rn>0) write(STDOUT_FILENO, buf, (size_t)rn);
            int isSel = (i == sel); if (isSel) write(STDOUT_FILENO, "\x1b[7m", 4);
            if (out_flags[i]) write(STDOUT_FILENO, "[\x1b[92mx\x1b[0m] ", 9); else write(STDOUT_FILENO, "[ ] ", 3);
            char labbuf[512]; snprintf(labbuf, sizeof(labbuf), "\x1b[96m%s\x1b[0m", labels[i]); write(STDOUT_FILENO, labbuf, strlen(labbuf));
            int pad = 2 + maxlabel - visible_len(labels[i]); for (int p = 0; p < pad; p++) write(STDOUT_FILENO, " ", 1);
            if (descs && descs[i]) { char descbuf[512]; snprintf(descbuf, sizeof(descbuf), "\x1b[90m%s\x1b[0m", descs[i]); write(STDOUT_FILENO, descbuf, strlen(descbuf)); }
            if (isSel) write(STDOUT_FILENO, "\x1b[m", 3);
        }
        int foot_ln = sy + boxh - 1; int foot_col = sx + 2; char foot[128]; int fp = snprintf(foot, sizeof(foot), "\x1b[%d;%dH\x1b[90mSpace toggles • Enter accepts • Esc cancels\x1b[0m", foot_ln, foot_col); if (fp>0) write(STDOUT_FILENO, foot, (size_t)fp);
        char cur[64]; int m = snprintf(cur, sizeof(cur), "\x1b[%d;%dH", sy + boxh, sx + 1); if (m>0) write(STDOUT_FILENO, cur, (size_t)m);
        int k = readKey(); if (k == -1) continue;
        if (k == ARROW_DOWN) { if (sel + 1 < n) sel++; if (sel >= end) { start++; end++; if (end > n) { end = n; start = end - rows; if (start < 0) start = 0; } } continue; }
        if (k == ARROW_UP) { if (sel > 0) sel--; if (sel < start) { start--; if (start < 0) start = 0; end = start + rows; if (end > n) end = n; } continue; }
        if (k == ' ') { out_flags[sel] = !out_flags[sel]; continue; }
        if (k == '\r' || k == '\n') { int cnt = 0; for (int i = 0; i < n; i++) if (out_flags[i]) cnt++; // restore screen and refresh before returning
            write(STDOUT_FILENO, "\x1b[2J", 4); write(STDOUT_FILENO, "\x1b[H", 3); editorRefreshScreen(); return cnt; }
        if (k == 27 || k == 'q' || k == 'Q') { return -1; }
    }
}

// -------------------------------------------------------------------------------------

// helper: open a file into the current editor window (push current HOME snapshot to front)

// Non-destructive variant: open into current window without pushing current buffer to a new tab.
static void open_file_in_current_window_no_push(const char *fname) {
    E.welcome_visible = 0;
    FILE *f = fopen(fname, "r");
    if (!f) {
        // prompt to create
        char __tmpbuf[64]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
        const char *fn_prompt2 = "\x1b[93mFile not found. Create?\x1b[0m \x1b[92m(y)\x1b[0m \x1b[91m(n)\x1b[0m ";
        write(STDOUT_FILENO, fn_prompt2, strlen(fn_prompt2));
        int resp = 0; while (1) { int k = readKey(); if (k == -1) continue; if (k=='y'||k=='Y') { resp='y'; break; } if (k=='n'||k=='N'||k==27) { resp='n'; break; } }
        __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows); if (__tmpn>0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
        if (resp != 'y') { snprintf(E.msg, sizeof(E.msg), "Open cancelled"); E.msg_time = time(NULL); return; }
        free(E.filename); E.filename = strdup(fname); free(E.saved_snapshot); E.saved_snapshot = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); set_language_from_filename(fname); snprintf(E.msg, sizeof(E.msg), "New file %s", fname); E.msg_time = time(NULL);
        /* Ensure new file opens at the top */
        E.cy = 0; E.cx = 0; E.row_offset = 0; E.col_offset = 0; editorScroll();
    } else {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = malloc(sz + 1); if (buf) { fread(buf, 1, sz, f); buf[sz] = '\0'; load_buffer_from_string(buf); free(buf); free(E.filename); E.filename = strdup(fname); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); set_language_from_filename(fname); snprintf(E.msg, sizeof(E.msg), "Opened %s", fname); E.msg_time = time(NULL);
            /* Ensure opened file shows from the top */
            E.cy = 0; E.cx = 0; E.row_offset = 0; E.col_offset = 0; editorScroll(); }
        fclose(f);
    }
}

// helper: open a file into a new tab (append and switch)
static void open_file_in_new_tab(const char *fname) {
    // create a new tab with the requested file loaded into it
    struct Tab t = {0};
    t.name = strdup(fname);
    // try to load file content into a temporary snapshot
    FILE *f = fopen(fname, "r");
    if (!f) {
        // new empty buffer
        t.snapshot = strdup("");
        t.saved_snapshot = NULL;
    } else {
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        char *buf = malloc(sz + 1);
        if (buf) { fread(buf, 1, sz, f); buf[sz] = '\0'; t.snapshot = strdup(buf); free(buf); }
        fclose(f);
        t.saved_snapshot = t.snapshot ? strdup(t.snapshot) : NULL;
    }
    /* Ensure new tab opens at the top (cursor and view) */
    t.cx = 0; t.cy = 0; t.row_offset = 0; t.col_offset = 0;
    /* OPEN_TAB_IMPL */
    int idx = append_tab(t);
    if (idx >= 0) switch_to_tab(idx);
}

// detect available build system in CWD
typedef enum { BS_NONE=0, BS_MAKE=1, BS_CMAKE=2, BS_CARGO=3 } BuildSystem;
static BuildSystem detect_build_system(char *outpath, int outpath_len) {
    char cwd[512]; getcwd(cwd, sizeof(cwd));
    (void)cwd;
    if (access("Makefile", F_OK) == 0) { if (outpath) strncpy(outpath, "Makefile", outpath_len); return BS_MAKE; }
    if (access("CMakeLists.txt", F_OK) == 0) { if (outpath) strncpy(outpath, "CMakeLists.txt", outpath_len); return BS_CMAKE; }
    if (access("Cargo.toml", F_OK) == 0) { if (outpath) strncpy(outpath, "Cargo.toml", outpath_len); return BS_CARGO; }
    return BS_NONE;
}

// parse Makefile for simple targets: lines of form "name:"
static char **get_make_targets(int *out_n) {
    *out_n = 0; FILE *f = fopen("Makefile", "r"); if (!f) return NULL;
    char line[512]; char **arr = NULL; int cnt = 0;
    while (fgets(line, sizeof(line), f)) {
        // ignore leading whitespace
        int i = 0; while (line[i] == ' ' || line[i] == '\t') i++;
        // target line if it contains ':' and no '=' before it
        char *c = strchr(&line[i], ':'); if (!c) continue;
        // ensure name is valid (no spaces)
        int len = (int)(c - &line[i]); if (len <= 0 || len > 128) continue;
        int ok = 1; for (int j = 0; j < len; j++) { char ch = line[i+j]; if (!(isalnum((unsigned char)ch) || ch=='_' || ch=='.' || ch=='-' )) { ok = 0; break; } }
        if (!ok) continue;
        char tmp[256]; int sl = len < (int)sizeof(tmp)-1 ? len : (int)sizeof(tmp)-1; memcpy(tmp, &line[i], sl); tmp[sl] = '\0';
        if (strcmp(tmp, ".PHONY") == 0) continue; // skip .PHONY: ...
        char **nar = realloc(arr, sizeof(char*) * (cnt + 1)); if (!nar) break; arr = nar; arr[cnt++] = strdup(tmp);
        if (cnt >= 200) break;
    }
    fclose(f);
    *out_n = cnt; return arr;
}

// blocking text input prompt at bottom of screen; returns allocated string (caller must free) or NULL on cancel
static char *prompt_input(const char *prompt) {
    char buf[512] = {0}; int len = 0;
    int row = E.screenrows;
    /* Print colored prompt: bold white message + dim hint that Enter cancels if nothing entered */
    char tmp[1024]; int pn = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H\x1b[K\x1b[1;97m%s\x1b[0m \x1b[90m<Enter to cancel>\x1b[0m \x1b[96m", row, prompt);
    if (pn>0) write(STDOUT_FILENO, tmp, (size_t)pn);
    while (1) {
        int k = readKey(); if (k == -1) continue;
        if (k == '\r' || k == '\n') { if (len == 0) { // nothing entered -> treat as cancel
                char clr[64]; int cn = snprintf(clr, sizeof(clr), "\x1b[%d;1H\x1b[K", row); if (cn>0) write(STDOUT_FILENO, clr, (size_t)cn); return NULL; }
            char *out = strdup(buf); char clr[64]; int cn = snprintf(clr, sizeof(clr), "\x1b[%d;1H\x1b[K", row); if (cn>0) write(STDOUT_FILENO, clr, (size_t)cn); return out; }
        if (k == 127 || k == 8) { if (len>0) { len--; buf[len] = '\0'; /* reflect back */ char clr[64]; int cn = snprintf(clr, sizeof(clr), "\x1b[%d;1H\x1b[K\x1b[1;97m%s\x1b[0m \x1b[90m<Enter to cancel>\x1b[0m \x1b[96m%s\x1b[0m", row, prompt, buf); if (cn>0) write(STDOUT_FILENO, clr, (size_t)cn); } continue; }
        if (k >= 32 && k <= 126) { if (len < (int)sizeof(buf)-1) { buf[len++] = (char)k; buf[len] = '\0'; /* reflect back */ char clr[64]; int cn = snprintf(clr, sizeof(clr), "\x1b[%d;1H\x1b[K\x1b[1;97m%s\x1b[0m \x1b[90m<Enter to cancel>\x1b[0m \x1b[96m%s\x1b[0m", row, prompt, buf); if (cn>0) write(STDOUT_FILENO, clr, (size_t)cn); } }
    }
} 

// ask yes/no prompt; returns 1 for yes, 0 for no
static int prompt_yesno(const char *msg) {
    int row = E.screenrows; char tmp[1024]; int pn = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H\x1b[K%s \x1b[92m(y)\x1b[0m / \x1b[91m(n)\x1b[0m: ", row, msg);
    if (pn>0) write(STDOUT_FILENO, tmp, (size_t)pn);
    int resp = 0; while (1) { int k = readKey(); if (k == -1) continue; if (k=='y'||k=='Y') { resp = 'y'; break; } if (k=='n'||k=='N'||k==27) { resp = 'n'; break; } }
    pn = snprintf(tmp, sizeof(tmp), "\x1b[%d;1H\x1b[K", row); if (pn>0) write(STDOUT_FILENO, tmp, (size_t)pn);
    return resp == 'y';
}

static void free_string_array(char **a, int n) { if (!a) return; for (int i=0;i<n;i++) free(a[i]); free(a); }

// simple per-project build config
typedef struct {
    char buildfile[256];
    char build_cmd[2048];
    char run_cmd[2048];
    time_t saved_at;
} BuildConfig;

// helper: extract a JSON string value for a simple flat JSON file
static int json_get_str(const char *buf, const char *key, char *out, int outlen) {
    if (!buf || !key || !out) return 0;
    char *p = strstr(buf, key);
    if (!p) return 0;
    p = strchr(p, ':'); if (!p) return 0; p++;
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p != '"') return 0; p++;
    int i = 0;
    while (*p && *p != '"' && i < outlen - 1) {
        if (*p == '\\' && *(p+1)) { p++; out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static int load_build_config(BuildConfig *cfg) {
    if (!cfg) return 0;
    FILE *f = fopen("config/build.json", "r"); if (!f) return 0;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    fread(buf, 1, sz, f); buf[sz] = '\0'; fclose(f);
    memset(cfg, 0, sizeof(*cfg));
    json_get_str(buf, "\"buildfile\"", cfg->buildfile, sizeof(cfg->buildfile));
    json_get_str(buf, "\"build_cmd\"", cfg->build_cmd, sizeof(cfg->build_cmd));
    json_get_str(buf, "\"run_cmd\"", cfg->run_cmd, sizeof(cfg->run_cmd));
    struct stat st; if (stat("config/build.json", &st) == 0) cfg->saved_at = st.st_mtime; else cfg->saved_at = time(NULL);
    free(buf);
    return 1;
}

static int save_build_config(const BuildConfig *cfg) {
    if (!cfg) return 0;
    // ensure config dir exists
    mkdir("config", 0755);
    FILE *f = fopen("config/build.json", "w"); if (!f) return 0;
    fprintf(f, "{\n");
    fprintf(f, "  \"buildfile\": \"%s\",\n", cfg->buildfile[0] ? cfg->buildfile : "");
    fprintf(f, "  \"build_cmd\": \"%s\",\n", cfg->build_cmd[0] ? cfg->build_cmd : "");
    fprintf(f, "  \"run_cmd\": \"%s\",\n", cfg->run_cmd[0] ? cfg->run_cmd : "");
    fprintf(f, "  \"saved_at\": %ld\n", (long)time(NULL));
    fprintf(f, "}\n");
    fclose(f);
    return 1;
}

// helper: find an executable by name in $PATH; returns 1 and writes path to out if found
static int find_in_path(const char *name, char *out, int outlen) {
    if (!name || !out) return 0;
    char *path = getenv("PATH"); if (!path) return 0;
    char *p = strdup(path);
    char *tok = strtok(p, ":");
    while (tok) {
        char fnbuf[512]; int n = snprintf(fnbuf, sizeof(fnbuf), "%s/%s", tok, name);
        if (n > 0 && n < (int)sizeof(fnbuf)) {
            if (access(fnbuf, X_OK) == 0) { strncpy(out, fnbuf, outlen-1); out[outlen-1] = '\0'; free(p); return 1; }
        }
        tok = strtok(NULL, ":");
    }
    free(p);
    return 0;
}

// helper: detect default compiler name/path for C or C++ (is_cxx=1 for C++)
// Behavior: prefer $CC/$CXX if set and found, then try cc/c++ then gcc/g++ then clang/clang++
static void detect_compiler(int is_cxx, char *out, int outlen) {
    if (!out || outlen <= 0) return;
    out[0] = '\0';
    const char *env = getenv(is_cxx ? "CXX" : "CC");
    if (env && env[0]) {
        // if env contains a slash, assume it's a path
        if (strchr(env, '/')) { if (access(env, X_OK) == 0) { strncpy(out, env, outlen-1); out[outlen-1] = '\0'; return; } }
        else {
            char full[512]; if (find_in_path(env, full, sizeof(full))) { strncpy(out, env, outlen-1); out[outlen-1] = '\0'; return; }
        }
    }
    // try standard commands in order
    const char *cands[] = { is_cxx ? "c++" : "cc", is_cxx ? "g++" : "gcc", is_cxx ? "clang++" : "clang", NULL };
    for (int i = 0; cands[i]; i++) {
        char full[512]; if (find_in_path(cands[i], full, sizeof(full))) { strncpy(out, cands[i], outlen-1); out[outlen-1] = '\0'; return; }
    }
    // fallback to generic names
    strncpy(out, is_cxx ? "c++" : "cc", outlen-1); out[outlen-1] = '\0';
}

// Probe whether the given compiler accepts a specific -std option for C/C++
static int probe_compile_check(const char *compiler, int is_cxx, const char *stdname) {
    if (!compiler || !stdname) return 0;
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "printf 'int main() { return 0; }' | %s -x %s - -std=%s -fsyntax-only -o /dev/null 2>/dev/null", compiler, is_cxx ? "c++" : "c", stdname);
    int st = system(cmd);
    return (st == 0);
}

// Probe whether the given compiler accepts an arbitrary flag (like -Wall, -fsanitize=address, -g, -pthread).
static int probe_flag_supported(const char *compiler, int is_cxx, const char *flag) {
    if (!compiler || !flag) return 0;
    char cmd[1024];
    /* Try a full compile/link to exercise flags that affect linking as well (e.g., -lm).
       Use -x c/c++ and pipe a tiny program to avoid disk I/O. */
    snprintf(cmd, sizeof(cmd), "printf 'int main() { return 0; }' | %s -x %s - %s -o /dev/null 2>/dev/null", compiler, is_cxx ? "c++" : "c", flag);
    int st = system(cmd);
    return (st == 0);
}

// Detect supported -std options (returns allocated array of strings; caller must free each and free the array)
static char **detect_supported_standards(const char *compiler, int is_cxx, int *out_n) {
    *out_n = 0; if (!compiler) return NULL;
    const char *c_stds[] = {"c89","c99","c11","c17","c2x", NULL};
    const char *cpp_stds[] = {"c++98","c++03","c++11","c++14","c++17","c++20","c++23", NULL};
    const char **cand = is_cxx ? cpp_stds : c_stds;
    char **res = NULL; int cnt = 0;
    for (int i = 0; cand[i]; i++) {
        if (probe_compile_check(compiler, is_cxx, cand[i])) {
            char *s = strdup(cand[i]); if (!s) continue;
            char **n = realloc(res, sizeof(char*) * (cnt + 1)); if (!n) { free(s); break; }
            res = n; res[cnt++] = s;
        }
    }
    *out_n = cnt; return res;
}



static void output_clear(void) {
    if (E.output_lines) {
        for (int i = 0; i < E.output_n; i++) free(E.output_lines[i]);
        free(E.output_lines); E.output_lines = NULL;
    }
    if (E.output_raw_lines) {
        for (int i = 0; i < E.output_n; i++) free(E.output_raw_lines[i]);
        free(E.output_raw_lines); E.output_raw_lines = NULL;
    }
    E.output_n = 0; E.output_scroll = 0; E.output_sel = 0;
}

// simple colorization for display only (log file remains raw)
static char *colorize_line_for_display(const char *line) {
    if (!line) return NULL;
    const char *RED = "\x1b[31m"; const char *YEL = "\x1b[33m"; const char *MAG = "\x1b[35m"; const char *RESET = "\x1b[0m";
    // prefer case-insensitive search if available
    if (strcasestr(line, "error:")) {
        size_t n = strlen(line) + 32; char *buf = malloc(n); if (!buf) return strdup(line); snprintf(buf, n, "%s%s%s", RED, line, RESET); return buf;
    } else if (strcasestr(line, "warning:")) {
        size_t n = strlen(line) + 32; char *buf = malloc(n); if (!buf) return strdup(line); snprintf(buf, n, "%s%s%s", YEL, line, RESET); return buf;
    } else if (strstr(line, "Run skipped:")) {
        size_t n = strlen(line) + 32; char *buf = malloc(n); if (!buf) return strdup(line); snprintf(buf, n, "%s%s%s", MAG, line, RESET); return buf;
    }
    return strdup(line);
}

static void output_append_colored_line(const char *rawline) {
    if (!rawline) return;
    // trim trailing newline
    size_t L = strlen(rawline);
    while (L > 0 && (rawline[L-1] == '\n' || rawline[L-1] == '\r')) L--;
    // store raw
    char *raw = malloc(L + 1); if (!raw) return; memcpy(raw, rawline, L); raw[L] = '\0';
    char **rn = realloc(E.output_raw_lines, sizeof(char*) * (E.output_n + 1)); if (!rn) { free(raw); return; }
    E.output_raw_lines = rn; E.output_raw_lines[E.output_n] = raw;
    // store colorized
    char *c = colorize_line_for_display(raw);
    char *col = c ? c : strdup(raw);
    char **n = realloc(E.output_lines, sizeof(char*) * (E.output_n + 1)); if (!n) { free(col); if (c) free(c); return; }
    E.output_lines = n; E.output_lines[E.output_n++] = col;
    // bound lines to avoid unbounded growth (keep last 5000)
    if (E.output_n > 5000) {
        free(E.output_raw_lines[0]); free(E.output_lines[0]);
        memmove(E.output_raw_lines, E.output_raw_lines + 1, sizeof(char*) * (E.output_n - 1));
        memmove(E.output_lines, E.output_lines + 1, sizeof(char*) * (E.output_n - 1));
        E.output_n--;
        char **rr = realloc(E.output_raw_lines, sizeof(char*) * E.output_n); if (rr) E.output_raw_lines = rr;
        char **r = realloc(E.output_lines, sizeof(char*) * E.output_n); if (r) E.output_lines = r;
        if (E.output_scroll > 0) E.output_scroll--;
        if (E.output_sel > 0) E.output_sel--;
    }
}

// Load `config/last_build.log` into the in-memory output pane if it changed.
static void output_load_last_log(void) {
    const char *path = "config/last_build.log";
    struct stat st;
    if (stat(path, &st) != 0) return; // no file
    if (E.output_last_mtime != 0 && E.output_last_mtime == st.st_mtime && E.output_n > 0) return; // no change
    FILE *f = fopen(path, "r"); if (!f) return;
    output_clear();
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        output_append_colored_line(line);
    }
    fclose(f);
    E.output_last_mtime = st.st_mtime;
    // auto-select the first error line (if any), otherwise default to top
    E.output_scroll = 0; E.output_sel = 0;
    for (int idx = 0; idx < E.output_n; idx++) {
        char pth[512]; int ln = 0, cn = 0;
        if (parse_error_location(E.output_raw_lines[idx], pth, sizeof(pth), &ln, &cn)) { E.output_sel = idx; E.output_scroll = idx; break; }
    }
}

// Append a raw (uncolored) line to the on-disk log, flush and fsync so it's durable
static void output_write_raw_to_log(const char *line) {
    if (!line) return;
    mkdir("config", 0755); // best-effort ensure dir exists
    FILE *f = fopen("config/last_build.log", "a"); if (!f) return;
    fputs(line, f); fputs("\n", f);
    fflush(f); fsync(fileno(f)); fclose(f);
    /* Force next render to reload from disk */
    E.output_last_mtime = 0;
}

// crude parser: find first substring of the form PATH:LINE[:COL]
// returns 1 and fills path/lineno/col if found
/* Strip ANSI escape sequences and other control characters from a string into a buffer.
   Returns the number of characters written (not including terminating NUL). */
static int strip_ansi(const char *in, char *out, int outlen) {
    if (!in || !out || outlen <= 0) return 0;
    int oi = 0; const unsigned char *p = (const unsigned char*)in;
    while (*p && oi < outlen - 1) {
        if (*p == 0x1b) {
            // skip ANSI CSI sequences: ESC '[' ... alpha
            p++;
            if (*p == '[') {
                p++;
                while (*p && oi < outlen - 1) {
                    if ((*p >= '@' && *p <= '~')) { p++; break; }
                    p++;
                }
                continue;
            }
            // skip other ESC sequences roughly
            while (*p && *p != 'm' && *p != '[' && oi < outlen - 1) p++;
            if (*p) p++;
            continue;
        }
        // drop control characters (except tab and space)
        if (*p < 32 && *p != '\t' && *p != '\n') { p++; continue; }
        out[oi++] = (char)*p++;
    }
    out[oi] = '\0'; return oi;
}

static int parse_error_location(const char *line, char *path, int pathlen, int *lineno, int *colno) {
    if (!line || !path || !lineno) return 0;
    const char *p = line;
    while (*p) {
        // look for ':' then digits
        const char *c = strchr(p, ':'); if (!c) break;
        // ensure there is at least one digit after ':'
        const char *d = c + 1; if (!isdigit((unsigned char)*d)) { p = c + 1; continue; }
        // backtrack to start of path token
        const char *s = c - 1; while (s > line && *s != ' ' && *s != '\t' && *s != '(' && *s != '"') s--; if (s != line) s++;
        int plen = (int)(c - s);
        if (plen <= 0 || plen >= pathlen) { p = c + 1; continue; }
        // copy path candidate
        char cand[512]; int cp = plen < (int)sizeof(cand)-1 ? plen : (int)sizeof(cand)-1; memcpy(cand, s, cp); cand[cp] = '\0';
        // sanitize candidate: strip ANSI/control chars and trim whitespace
        char tmpcand[512]; strip_ansi(cand, tmpcand, sizeof(tmpcand));
        char *tc = tmpcand; while (*tc && (unsigned char)*tc <= 32) tc++;
        size_t tl = strlen(tc); while (tl > 0 && (unsigned char)tc[tl-1] <= 32) tc[--tl] = '\0';
        // move cleaned string back into cand for existing checks
        if (tc != tmpcand) memmove(cand, tc, tl + 1); else strncpy(cand, tmpcand, sizeof(cand)-1), cand[sizeof(cand)-1] = '\0';
        // parse number after ':'
        int ln = 0; int col = 0; int consumed = 0;
        if (sscanf(c + 1, "%d%n", &ln, &consumed) != 1) { p = c + 1; continue; }
        const char *after = c + 1 + consumed;
        if (*after == ':') {
            int cc = 0; int cons2 = 0; if (sscanf(after + 1, "%d%n", &cc, &cons2) == 1) { col = cc; after = after + 1 + cons2; }
        }
        // If current buffer was the one built/selected, prefer it when basenames match
        if (E.filename && E.filename[0]) {
            char *bn = strrchr(E.filename, '/'); const char *efn = bn ? bn + 1 : E.filename;
            if (strcmp(efn, cand) == 0 || strcmp(E.filename, cand) == 0) {
                strncpy(path, E.filename, pathlen-1); path[pathlen-1] = '\0'; *lineno = ln; if (colno) *colno = col; return 1;
            }
        }
        // verify the path exists (attempt) — try as-is and also try relative path (same dir)
        if (access(cand, R_OK) == 0) { strncpy(path, cand, pathlen-1); path[pathlen-1] = '\0'; *lineno = ln; if (colno) *colno = col; return 1; }
        // try trimming leading './'
        if (strncmp(cand, "./", 2) == 0) {
            if (access(cand + 2, R_OK) == 0) { strncpy(path, cand + 2, pathlen-1); path[pathlen-1] = '\0'; *lineno = ln; if (colno) *colno = col; return 1; }
        }
        // maybe path is relative without dir; try as basename matching in cwd
        DIR *dirp = opendir("."); if (dirp) {
            struct dirent *de; while ((de = readdir(dirp))) {
                if (strcmp(de->d_name, cand) == 0) { strncpy(path, de->d_name, pathlen-1); path[pathlen-1] = '\0'; closedir(dirp); *lineno = ln; if (colno) *colno = col; return 1; }
            }
            closedir(dirp);
        }
        // also try matching against open tabs (full path or basename)
        if (Tabs) {
            for (int t = 0; t < NumTabs; t++) {
                if (!Tabs[t].name) continue;
                const char *tn = Tabs[t].name;
                if (strcmp(tn, cand) == 0) { strncpy(path, tn, pathlen-1); path[pathlen-1] = '\0'; *lineno = ln; if (colno) *colno = col; return 1; }
                char *bn = strrchr(tn, '/'); const char *bname = bn ? bn + 1 : tn;
                if (strcmp(bname, cand) == 0) { strncpy(path, tn, pathlen-1); path[pathlen-1] = '\0'; *lineno = ln; if (colno) *colno = col; return 1; }
            }
        }
        p = c + 1;
    }
    return 0;
}

// interactive setup for projects without a detected build system
// fills outcfg and returns 1 on success, 0 on cancel/failure
static int interactive_setup(BuildConfig *outcfg) {
    if (!outcfg) return 0;
    char **cands = NULL; int nc = 0;
    DIR *d = opendir("."); if (!d) { snprintf(E.msg, sizeof(E.msg), "Cannot open cwd: %s", strerror(errno)); E.msg_time = time(NULL); return 0; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        struct stat st; if (stat(de->d_name, &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) continue;
        const char *ext = strrchr(de->d_name, '.');
        if (!ext) continue;
        if (strcmp(ext, ".c") == 0 || strcmp(ext, ".cpp") == 0 || strcmp(ext, ".rs") == 0 || strcmp(ext, ".go") == 0 || strcmp(ext, ".py") == 0 || strcmp(ext, ".java") == 0 || strcmp(ext, ".js") == 0) {
            char **n = realloc(cands, sizeof(char*) * (nc + 1)); if (!n) continue; cands = n; cands[nc++] = strdup(de->d_name);
        }
    }
    closedir(d);
    if (nc == 0) { snprintf(E.msg, sizeof(E.msg), "No candidate source files found"); E.msg_time = time(NULL); return 0; }
    int idx = pick_from_matches(cands, nc);
    if (idx < 0) { free_string_array(cands, nc); return 0; }
    char *src = strdup(cands[idx]); free_string_array(cands, nc);
    memset(outcfg, 0, sizeof(*outcfg)); strncpy(outcfg->buildfile, src, sizeof(outcfg->buildfile)-1);
    // heuristics based on extension
    int lang = detect_language(src);
    char exe_default[128]; snprintf(exe_default, sizeof(exe_default), "%s", src);
    char *dot = strrchr(exe_default, '.'); if (dot) *dot = '\0';
    char exe_name[256]; exe_name[0] = '\0';
    if (lang == LANG_NONE) {
        // fallback
        char *b = prompt_input("Enter build command (empty to skip): "); if (b) { strncpy(outcfg->build_cmd, b, sizeof(outcfg->build_cmd)-1); free(b); }
        char *r = prompt_input("Enter run command (empty to skip): "); if (r) { strncpy(outcfg->run_cmd, r, sizeof(outcfg->run_cmd)-1); free(r); }
    } else {
        const LangDef *ld = &lang_defs[lang - 1];
        if (lang == LANG_C || lang == LANG_CPP) {
        int is_cxx = (lang == LANG_CPP);
        char compiler_name[256] = {0}; detect_compiler(is_cxx, compiler_name, sizeof(compiler_name));
        // allow user to override detected compiler if desired
        char probe_msg[512]; snprintf(probe_msg, sizeof(probe_msg), "Use detected compiler %s?", compiler_name);
        if (!prompt_yesno(probe_msg)) {
            char *in = prompt_input("Enter compiler (name or path): "); if (!in) { free(src); return 0; } strncpy(compiler_name, in, sizeof(compiler_name)-1); free(in);
        }
        // probe supported standards
        int std_n = 0; char **stds = detect_supported_standards(compiler_name, is_cxx, &std_n);
        int std_idx = -1;
        if (std_n > 0) {
            std_idx = pick_from_matches((char**)stds, std_n);
            if (std_idx < 0) { for (int ii=0; ii<std_n; ii++) free(stds[ii]); free(stds); free(src); return 0; }
        } else {
            // fallback static list
            const char *defaults[] = { is_cxx ? "c++17" : "c99", is_cxx ? "c++20" : "c11", is_cxx ? "c++23" : "c2x", NULL };
            // copy defaults into dynamic array for pick_from_matches
            int di = 0; while (defaults[di]) di++;
            char **dlist = malloc(sizeof(char*) * di);
            for (int i = 0; i < di; i++) dlist[i] = strdup(defaults[i]);
            std_idx = pick_from_matches(dlist, di);
            if (std_idx < 0) { for (int i = 0; i < di; i++) free(dlist[i]); free(dlist); free(src); return 0; }
            // convert selected into stds so downstream cleanup is uniform
            stds = dlist; std_n = di;
        }
        const char *chosen_std = stds[std_idx];
        // warnings menu (multi-select via numeric input)
        /* Warning flags: probe compiler support and present a multi-select chooser with descriptions */
        const char *warn_opts[] = {"-Wall","-Wextra","-Werror","-Wpedantic"};
        const char *warn_descs[] = {"Enable common warnings","Additional useful warnings","Treat warnings as errors","Extra pedantic checks"};
        int warn_count = 4;
        char *wlabels[8]; char *wdescs[8]; int wcnt = 0; int wflags[8] = {0};
        for (int i = 0; i < warn_count; i++) {
            if (probe_flag_supported(compiler_name, is_cxx, warn_opts[i])) {
                wlabels[wcnt] = strdup(warn_opts[i]); wdescs[wcnt] = strdup(warn_descs[i]); wcnt++;
            }
        }
        if (wcnt == 0) { // fallback: include defaults
            for (int i = 0; i < warn_count && i < 8; i++) { wlabels[wcnt] = strdup(warn_opts[i]); wdescs[wcnt] = strdup(warn_descs[i]); wcnt++; }
        }
        // default selection: -Wall + -Wextra if available
        for (int i = 0; i < wcnt; i++) { if (strcmp(wlabels[i], "-Wall") == 0 || strcmp(wlabels[i], "-Wextra") == 0) wflags[i] = 1; }
        // show picker
        int sel = pick_multi_from_matches_with_desc(wlabels, wdescs, wcnt, wflags);
        if (sel < 0) { // cancelled; free and abort
            for (int i=0;i<wcnt;i++) { free(wlabels[i]); free(wdescs[i]); }
            for (int ii = 0; ii < std_n; ii++) free(stds[ii]); free(stds); free(src); return 0; }
        char warnstr[256] = {0}; for (int i=0;i<wcnt;i++) if (wflags[i]) { if (warnstr[0]) strncat(warnstr, " ", sizeof(warnstr)-strlen(warnstr)-1); strncat(warnstr, wlabels[i], sizeof(warnstr)-strlen(warnstr)-1); }
        for (int i=0;i<wcnt;i++) { free(wlabels[i]); free(wdescs[i]); }
        // optimization menu (single choice)
        const char *opt_opts[] = {"-O0","-O1","-O2","-O3"}; int optc = 4; int opti = pick_from_matches((char**)opt_opts, optc); if (opti < 0) opti = 2; const char *optflag = opt_opts[opti];
        // Extra flags: probe common extras and present multi-select including 'Custom...'
        const char *extra_opts[] = {"-g","-fsanitize=address","-march=native","-pthread","-lm","Custom..."}; int extra_cand = 6;
        char *elabels[8]; char *edescs[8]; int ecnt = 0; int eflags[8] = {0};
        const char *extra_descs[] = {"Include debug symbols","AddressSanitizer (memory checks)","CPU-specific tuning","Enable pthreads","Link math library","Type custom flags"};
        for (int i = 0; i < extra_cand - 1; i++) {
            if (probe_flag_supported(compiler_name, is_cxx, extra_opts[i])) {
                elabels[ecnt] = strdup(extra_opts[i]); edescs[ecnt] = strdup(extra_descs[i]); ecnt++;
            }
        }
        // always add Custom... option
        elabels[ecnt] = strdup("Custom..."); edescs[ecnt] = strdup("Type custom flags (space-separated)"); ecnt++;
        int ecount = pick_multi_from_matches_with_desc(elabels, edescs, ecnt, eflags);
        char extrabuf[256] = {0};
        if (ecount > 0) {
            for (int i=0;i<ecnt;i++) {
                if (!eflags[i]) continue;
                if (strcmp(elabels[i], "Custom...") == 0) {
                    char *custom = prompt_input("Custom extra flags (space-separated, optional): ");
                    if (custom && custom[0]) { strncat(extrabuf, custom, sizeof(extrabuf)-strlen(extrabuf)-1); strncat(extrabuf, " ", sizeof(extrabuf)-strlen(extrabuf)-1); }
                    if (custom) free(custom);
                } else {
                    strncat(extrabuf, elabels[i], sizeof(extrabuf)-strlen(extrabuf)-1);
                    strncat(extrabuf, " ", sizeof(extrabuf)-strlen(extrabuf)-1);
                }
            }
        }
        for (int i=0;i<ecnt;i++) { free(elabels[i]); free(edescs[i]); }
        // determine executable name
        char *en = prompt_input("Executable name (leave empty for default): "); if (en) { if (en[0]) strncpy(exe_name, en, sizeof(exe_name)-1); free(en); }
        if (!exe_name[0]) strncpy(exe_name, exe_default, sizeof(exe_name)-1);
        // assemble build command
        char buildbuf[1024] = {0}; snprintf(buildbuf, sizeof(buildbuf), "%s -std=%s %s %s -o %s %s", compiler_name, chosen_std, warnstr, optflag, exe_name, src);
        if (extrabuf[0]) { strncat(buildbuf, " ", sizeof(buildbuf)-strlen(buildbuf)-1); strncat(buildbuf, extrabuf, sizeof(buildbuf)-strlen(buildbuf)-1); }
        strncpy(outcfg->build_cmd, buildbuf, sizeof(outcfg->build_cmd)-1);
        snprintf(outcfg->run_cmd, sizeof(outcfg->run_cmd), "./%s", exe_name);
        // cleanup
        for (int ii = 0; ii < std_n; ii++) { free(stds[ii]); }
        free(stds); stds = NULL;
    } else {
        // general for other languages
        char build_tool[256] = {0};
        char run_tool[256] = {0};
        if (ld->build_tools) {
            detect_tool(ld->build_tools, ld->build_tool_count, build_tool, sizeof(build_tool));
        }
        if (ld->run_tools) {
            detect_tool(ld->run_tools, ld->run_tool_count, run_tool, sizeof(run_tool));
        }
        if (!build_tool[0] && !run_tool[0]) {
            char msg[2048];
            const char **tools = ld->build_tools ? ld->build_tools : ld->run_tools;
            int tcount = ld->build_tools ? ld->build_tool_count : ld->run_tool_count;
            if (tcount > 0) {
                snprintf(msg, sizeof(msg), "Tool not found. Install %s or enter manually?", tools[0]);
            } else {
                strcpy(msg, "No tool available for this language. Enter commands manually?");
            }
            if (!prompt_yesno(msg)) {
                // manual
                char *b = prompt_input("Enter build command (empty to skip): "); if (b) { strncpy(outcfg->build_cmd, b, sizeof(outcfg->build_cmd)-1); free(b); }
                char *r = prompt_input("Enter run command (empty to skip): "); if (r) { strncpy(outcfg->run_cmd, r, sizeof(outcfg->run_cmd)-1); free(r); }
            } else {
                // hint
                if (tcount > 0) {
                    snprintf(E.msg, sizeof(E.msg), "Install %s first", tools[0]);
                } else {
                    snprintf(E.msg, sizeof(E.msg), "No tool for %s", ld->name);
                }
                E.msg_time = time(NULL);
                free(src);
                return 0;
            }
        } else {
            // have tool
            if (ld->build_template[0]) {
                char *en = prompt_input("Executable name (leave empty for default): "); if (en) { if (en[0]) strncpy(exe_name, en, sizeof(exe_name)-1); free(en); }
                if (!exe_name[0]) strncpy(exe_name, exe_default, sizeof(exe_name)-1);
            }
            // flags
            char flagstr[1024] = {0};
            if (ld->has_flags && build_tool[0]) {
                char **flags = NULL;
                int nflags = 0;
                if (parse_flags_from_help(build_tool, &flags, &nflags)) {
                    if (nflags > 0) {
                        char **descs = calloc(nflags, sizeof(char*));
                        for (int i = 0; i < nflags; i++) descs[i] = strdup("");
                        int fflags[64] = {0};
                        int sel = pick_multi_from_matches_with_desc(flags, descs, nflags, fflags);
                        for (int i = 0; i < nflags; i++) free(descs[i]);
                        free(descs);
                        if (sel < 0) {
                            free_string_array(flags, nflags);
                            free(src);
                            return 0;
                        }
                        for (int i = 0; i < nflags; i++) {
                            if (fflags[i]) {
                                if (flagstr[0]) strncat(flagstr, " ", sizeof(flagstr)-strlen(flagstr)-1);
                                strncat(flagstr, flags[i], sizeof(flagstr)-strlen(flagstr)-1);
                            }
                        }
                    }
                }
                free_string_array(flags, nflags);
            }
            // build cmd
            if (ld->build_template[0]) {
                char buildbuf[1024] = {0};
                if (strcmp(ld->name, "Java") == 0) {
                    snprintf(buildbuf, sizeof(buildbuf), ld->build_template, build_tool, src);
                } else if (strcmp(ld->name, "Go") == 0) {
                    snprintf(buildbuf, sizeof(buildbuf), ld->build_template, build_tool, flagstr, exe_name, src);
                } else if (strcmp(ld->name, "Zig") == 0) {
                    snprintf(buildbuf, sizeof(buildbuf), ld->build_template, build_tool, src, exe_name);
                } else if (strcmp(ld->name, "C#") == 0) {
                    snprintf(buildbuf, sizeof(buildbuf), ld->build_template, build_tool, src, exe_name);
                } else if (strcmp(ld->name, "Rust") == 0) {
                    // Rust expects (src, exe, flags)
                    snprintf(buildbuf, sizeof(buildbuf), ld->build_template, src, exe_name, flagstr);
                } else {
                    // C, C++
                    snprintf(buildbuf, sizeof(buildbuf), ld->build_template, build_tool, src, exe_name, flagstr);
                }
                strncpy(outcfg->build_cmd, buildbuf, sizeof(outcfg->build_cmd)-1);
            }
            // run cmd
            if (ld->run_template[0]) {
                char runbuf[256] = {0};
                char *tool_for_run = run_tool[0] ? run_tool : build_tool;
                if (strcmp(ld->name, "Java") == 0) {
                    char class[128];
                    char temp[256];
                    strcpy(temp, src);
                    char *dot = strrchr(temp, '.');
                    if (dot) *dot = '\0';
                    char *slash = strrchr(temp, '/');
                    if (slash) strcpy(class, slash+1);
                    else strcpy(class, temp);
                    snprintf(runbuf, sizeof(runbuf), ld->run_template, tool_for_run, class);
                } else if (strstr(ld->run_template, "%s %s")) {
                    // interpreted
                    snprintf(runbuf, sizeof(runbuf), ld->run_template, tool_for_run, src);
                } else {
                    // compiled
                    snprintf(runbuf, sizeof(runbuf), ld->run_template, exe_name);
                }
                strncpy(outcfg->run_cmd, runbuf, sizeof(outcfg->run_cmd)-1);
            }
        }
    }
}

    // ask to save
    if (prompt_yesno("Save this configuration for future builds?")) {
        save_build_config(outcfg);
        snprintf(E.msg, sizeof(E.msg), "Saved build config"); E.msg_time = time(NULL);
    }

    free(src);
    return 1;
}

// choose a target/command then run or set for post-save
static void handle_build_run(int do_run_after);
static void handle_run_only(int allow_interactive, const char *arg);

// check if a tool is available in PATH
static int tool_available(const char *tool) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "command -v %s > /dev/null 2>&1", tool);
    return system(cmd) == 0;
}

// run an executable directly (for 'run' command, skips build detection)
static int run_interactive(const char *fname) {
    char cmd[4096];
    const char *ext = strrchr(fname, '.');
    char base[512];
    strncpy(base, fname, sizeof(base)-1);
    char *dot = strrchr(base, '.');
    if (dot) *dot = '\0';
    char *slash = strrchr(base, '/');
    char *bname = slash ? slash + 1 : base;
    char exe[2048];
    snprintf(exe, sizeof(exe), "./%s", bname);
    if (ext) {
        if (strcmp(ext, ".py") == 0) {
            snprintf(cmd, sizeof(cmd), "python3 '%s'", fname);
        } else if (strcmp(ext, ".sh") == 0) {
            snprintf(cmd, sizeof(cmd), "bash '%s'", fname);
        } else if (strcmp(ext, ".pl") == 0) {
            snprintf(cmd, sizeof(cmd), "perl '%s'", fname);
        } else if (strcmp(ext, ".rb") == 0) {
            snprintf(cmd, sizeof(cmd), "ruby '%s'", fname);
        } else if (strcmp(ext, ".js") == 0) {
            snprintf(cmd, sizeof(cmd), "node '%s'", fname);
        } else if (strcmp(ext, ".java") == 0) {
            /* For Java, compile if needed */
            char classfile[2048];
            snprintf(classfile, sizeof(classfile), "%s.class", bname);
            struct stat st_src, st_class;
            int need_compile = 1;
            if (stat(fname, &st_src) == 0 && stat(classfile, &st_class) == 0) {
                if (st_class.st_mtime >= st_src.st_mtime) need_compile = 0;
            }
            if (need_compile) {
                if (!tool_available("javac")) {
                    char msg[2048];
                    snprintf(msg, sizeof(msg), "Tool not found. Install javac or enter manually?");
                    if (prompt_yesno(msg)) {
                        snprintf(E.msg, sizeof(E.msg), "Install javac first");
                        E.msg_time = time(NULL);
                        return -1;
                    } else {
                        char *manual = prompt_input("Enter compile command (e.g., javac file.java): ");
                        if (manual && manual[0]) {
                            char runcmd[4096];
                            snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", manual);
                            int rc = execute_command(runcmd);
                            free(manual);
                            if (rc != 0) return rc;
                        } else {
                            if (manual) free(manual);
                            return -1;
                        }
                    }
                } else {
                    char compile_cmd[1024];
                    snprintf(compile_cmd, sizeof(compile_cmd), "PATH=\"$PATH\" javac '%s'", fname);
                    char runcmd[4096];
                    snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", compile_cmd);
                    int compile_exit = execute_command(runcmd);
                    if (compile_exit != 0) {
                        return compile_exit;
                    }
                }
            }
            snprintf(cmd, sizeof(cmd), "java %s", bname);
        } else if (strcmp(ext, ".rs") == 0) {
            // Rust
            struct stat st_src, st_exe;
            int need_compile = 1;
            if (stat(fname, &st_src) == 0 && stat(exe, &st_exe) == 0) {
                if (st_exe.st_mtime >= st_src.st_mtime) need_compile = 0;
            }
            if (need_compile) {
                if (!tool_available("rustc")) {
                    char msg[2048];
                    snprintf(msg, sizeof(msg), "Tool not found. Install rustc or enter manually?");
                    if (prompt_yesno(msg)) {
                        snprintf(E.msg, sizeof(E.msg), "Install rustc first");
                        E.msg_time = time(NULL);
                        return -1;
                    } else {
                        char *manual = prompt_input("Enter compile command (e.g., rustc file.rs -o exe): ");
                        if (manual && manual[0]) {
                            char runcmd[4096];
                            snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", manual);
                            int rc = execute_command(runcmd);
                            free(manual);
                            if (rc != 0) return rc;
                        } else {
                            if (manual) free(manual);
                            return -1;
                        }
                    }
                } else {
                    char compile_cmd[1024];
                    snprintf(compile_cmd, sizeof(compile_cmd), "PATH=\"$PATH\" rustc '%s' -o '%s'", fname, bname);
                    char runcmd[4096];
                    snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", compile_cmd);
                    int compile_exit = execute_command(runcmd);
                    if (compile_exit != 0) {
                        return compile_exit;
                    }
                }
            }
            snprintf(cmd, sizeof(cmd), "'%s'", exe);
        } else if (strcmp(ext, ".c") == 0) {
            // C
            struct stat st_src, st_exe;
            int need_compile = 1;
            if (stat(fname, &st_src) == 0 && stat(exe, &st_exe) == 0) {
                if (st_exe.st_mtime >= st_src.st_mtime) need_compile = 0;
            }
            if (need_compile) {
                if (!tool_available("gcc")) {
                    char msg[2048];
                    snprintf(msg, sizeof(msg), "Tool not found. Install gcc or enter manually?");
                    if (prompt_yesno(msg)) {
                        snprintf(E.msg, sizeof(E.msg), "Install gcc first");
                        E.msg_time = time(NULL);
                        return -1;
                    } else {
                        char *manual = prompt_input("Enter compile command (e.g., gcc file.c -o exe): ");
                        if (manual && manual[0]) {
                            char runcmd[4096];
                            snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", manual);
                            int rc = execute_command(runcmd);
                            free(manual);
                            if (rc != 0) return rc;
                        } else {
                            if (manual) free(manual);
                            return -1;
                        }
                    }
                } else {
                    char compile_cmd[1024];
                    snprintf(compile_cmd, sizeof(compile_cmd), "PATH=\"$PATH\" gcc '%s' -o '%s'", fname, bname);
                    char runcmd[4096];
                    snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", compile_cmd);
                    int compile_exit = execute_command(runcmd);
                    if (compile_exit != 0) {
                        return compile_exit;
                    }
                }
            }
            snprintf(cmd, sizeof(cmd), "'%s'", exe);
        } else if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0 || strcmp(ext, ".cxx") == 0) {
            // C++
            struct stat st_src, st_exe;
            int need_compile = 1;
            if (stat(fname, &st_src) == 0 && stat(exe, &st_exe) == 0) {
                if (st_exe.st_mtime >= st_src.st_mtime) need_compile = 0;
            }
            if (need_compile) {
                if (!tool_available("g++")) {
                    char msg[2048];
                    snprintf(msg, sizeof(msg), "Tool not found. Install g++ or enter manually?");
                    if (prompt_yesno(msg)) {
                        snprintf(E.msg, sizeof(E.msg), "Install g++ first");
                        E.msg_time = time(NULL);
                        return -1;
                    } else {
                        char *manual = prompt_input("Enter compile command (e.g., g++ file.cpp -o exe): ");
                        if (manual && manual[0]) {
                            char runcmd[4096];
                            snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", manual);
                            int rc = execute_command(runcmd);
                            free(manual);
                            if (rc != 0) return rc;
                        } else {
                            if (manual) free(manual);
                            return -1;
                        }
                    }
                } else {
                    char compile_cmd[1024];
                    snprintf(compile_cmd, sizeof(compile_cmd), "PATH=\"$PATH\" g++ '%s' -o '%s'", fname, bname);
                    char runcmd[4096];
                    snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", compile_cmd);
                    int compile_exit = execute_command(runcmd);
                    if (compile_exit != 0) {
                        return compile_exit;
                    }
                }
            }
            snprintf(cmd, sizeof(cmd), "'%s'", exe);
        } else {
            /* Other extensions: assume compiled, run ./basename */
            snprintf(cmd, sizeof(cmd), "'%s'", exe);
        }
    } else {
        /* No extension: if executable, run directly; otherwise try ./basename */
        struct stat st;
        if (stat(fname, &st) == 0 && (st.st_mode & S_IXUSR)) {
            snprintf(cmd, sizeof(cmd), "'%s'", fname);
        } else {
            snprintf(cmd, sizeof(cmd), "'%s'", exe);
        }
    }
    char runcmd[8192];
    snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", cmd);
    return execute_command(runcmd);
}

static void handle_build_run(int do_run_after) {
    // ensure a fresh log file for build/run operations
    (void)unlink("config/last_build.log");

    char path[256] = {0}; BuildSystem bs = detect_build_system(path, sizeof(path));
    if (bs != BS_NONE) {
        // offer options: use build system, setup for current file, run current file directly
        const char *opts[] = {"Use build system", "Setup for current file", "Run current file directly"};
        int optc = 3;
        int idx = pick_from_matches((char**)opts, optc);
        if (idx < 0) { snprintf(E.msg, sizeof(E.msg), "Cancelled"); E.msg_time = time(NULL); return; }
        if (idx == 0) {
            // use build system - original logic
            char q[512]; int pn = snprintf(q, sizeof(q), "Edit %s?", path);
            if (prompt_yesno(q)) {
                // let user pick which target to run after save
                char *selcmd = NULL;
                if (bs == BS_MAKE) {
                    int tn=0; char **t = get_make_targets(&tn);
                    if (tn>0) {
                        int idx2 = pick_from_matches(t, tn);
                        if (idx2 >= 0) { selcmd = malloc(256); snprintf(selcmd, 256, "make %s", t[idx2]); }
                        free_string_array(t, tn);
                    }
                } else if (bs == BS_CARGO) {
                    const char *opts2[] = {"build","run","test","clean"}; int optc2 = 4;
                    int idx2 = pick_from_matches((char**)opts2, optc2);
                    if (idx2 >= 0) { selcmd = malloc(256); snprintf(selcmd, 256, "cargo %s", opts2[idx2]); }
                } else if (bs == BS_CMAKE) {
                    const char *opts2[] = {"build","clean","test"}; int optc2 = 3;
                    int idx2 = pick_from_matches((char**)opts2, optc2);
                    if (idx2 >= 0) { if (strcmp(opts2[idx2], "build") == 0) snprintf((selcmd=malloc(256)),256, "cmake --build ."); else snprintf((selcmd=malloc(256)),256, "cmake --build . --target %s", opts2[idx2]); }
                }
                if (!selcmd) { // allow custom
                    char *in = prompt_input("Enter custom command (empty to cancel): "); if (in) { selcmd = in; } 
                }
                if (selcmd) {
                    // set post-edit command and open file for editing
                    strncpy(E.post_edit_cmd, selcmd, sizeof(E.post_edit_cmd)-1); E.post_edit_cmd[sizeof(E.post_edit_cmd)-1] = '\0';
                    E.awaiting_buildfile_save = 1; free(selcmd);
                    open_file_in_current_window_no_push(path);
                    return;
                } else {
                    snprintf(E.msg, sizeof(E.msg), "No command selected"); E.msg_time = time(NULL); return;
                }
            } else {
                // don't edit: pick target and run now
                char *cmd = NULL;
                if (bs == BS_MAKE) {
                    int tn=0; char **t = get_make_targets(&tn);
                    if (tn>0) {
                        int idx2 = pick_from_matches(t, tn);
                        if (idx2 >= 0) { cmd = malloc(256); snprintf(cmd, 256, "make %s", t[idx2]); }
                        free_string_array(t, tn);
                    }
                } else if (bs == BS_CARGO) {
                    const char *opts2[] = {"build","run","test","clean"}; int optc2 = 4;
                    int idx2 = pick_from_matches((char**)opts2, optc2);
                    if (idx2 >= 0) { cmd = malloc(256); snprintf(cmd, 256, "cargo %s", opts2[idx2]); }
                } else if (bs == BS_CMAKE) {
                    const char *opts2[] = {"build","clean","test"}; int optc2 = 3;
                    int idx2 = pick_from_matches((char**)opts2, optc2);
                    if (idx2 >= 0) { cmd = malloc(256); if (strcmp(opts2[idx2], "build") == 0) snprintf(cmd,256, "cmake --build ."); else snprintf(cmd,256, "cmake --build . --target %s", opts2[idx2]); }
                }
                if (!cmd) { char *in = prompt_input("Enter custom command (empty to cancel): "); if (in) { cmd = in; } }
                if (cmd) {
                    char runcmd[768]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", cmd);
                    int st = execute_command(runcmd);
                    if (do_run_after) {
                        // If the selected command already contains 'run' we assume it executed the run step.
                        if (!strstr(cmd, "run")) {
                            // Otherwise, attempt to use saved project run command if present and only run on successful build
                            BuildConfig cfg2 = {0}; if (load_build_config(&cfg2) && cfg2.run_cmd[0] && st == 0) {
                                char rc[4096]; snprintf(rc, sizeof(rc), "__run_shell__:%s", cfg2.run_cmd); execute_command(rc);
                            }
                        }
                    }
                    free(cmd);
                    return;
                } else { snprintf(E.msg, sizeof(E.msg), "Command cancelled"); E.msg_time = time(NULL); return; }
            }
        } else if (idx == 1) {
            // setup for current file
            if (E.filename && E.filename[0]) {
                BuildConfig nc = {0};
                // set buildfile to current file
                strncpy(nc.buildfile, E.filename, sizeof(nc.buildfile)-1);
                if (interactive_setup(&nc)) {
                    // run the build (and run if requested)
                    int st = 0;
                    if (nc.build_cmd[0]) { char bcmd[4096]; snprintf(bcmd, sizeof(bcmd), "__run_shell__:%s", nc.build_cmd); st = execute_command(bcmd); }
                    if (do_run_after && nc.run_cmd[0]) {
                        if (st == 0) {
                            char rc[4096]; snprintf(rc, sizeof(rc), "__run_shell__:%s", nc.run_cmd); execute_command(rc);
                        } else {
                            char emsg[256]; snprintf(emsg, sizeof(emsg), "Run skipped: build failed (exit %d)", st);
                            output_append_colored_line(emsg);
                            output_write_raw_to_log(emsg);
                            E.output_visible = 1; E.output_scroll = 0; E.output_sel = 0; editorRefreshScreen();
                        }
                    }
                    return;
                } else {
                    snprintf(E.msg, sizeof(E.msg), "Setup cancelled"); E.msg_time = time(NULL);
                }
            } else {
                snprintf(E.msg, sizeof(E.msg), "No current file"); E.msg_time = time(NULL);
            }
        } else if (idx == 2) {
            // run current file directly (use unified run_interactive)
            if (E.filename && E.filename[0]) {
                int rc = run_interactive(E.filename);
                if (rc == 0) return;
                else if (rc != -1) { snprintf(E.msg, sizeof(E.msg), "Run finished (exit %d)", rc); E.msg_time = time(NULL); }
            } else {
                snprintf(E.msg, sizeof(E.msg), "No current file"); E.msg_time = time(NULL);
            }
        }
    } else {
        // no build system: try saved config
        BuildConfig cfg = {0};
        if (load_build_config(&cfg)) {
            if (prompt_yesno("Saved build config found. Use it?")) {
                if (cfg.build_cmd[0]) {
                    char runcmd[4096]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", cfg.build_cmd);
                    int st = execute_command(runcmd);
                    if (do_run_after && cfg.run_cmd[0]) {
                        if (st == 0) {
                            char rc[4096]; snprintf(rc, sizeof(rc), "__run_shell__:%s", cfg.run_cmd); execute_command(rc);
                        } else {
                            char emsg[256]; snprintf(emsg, sizeof(emsg), "Run skipped: build failed (exit %d)", st);
                            output_append_colored_line(emsg);
                            output_write_raw_to_log(emsg);
                            E.output_visible = 1; E.output_scroll = 0; E.output_sel = 0; editorRefreshScreen();
                        }
                    }
                    return;
                }
            }
        }
        // fallback to interactive flow
        if (prompt_yesno("No build system found. Run interactive setup?")) {
            BuildConfig nc = {0};
            if (interactive_setup(&nc)) {
                // after setup, run the build (and run if requested)
                int st = 0;
                if (nc.build_cmd[0]) { char bcmd[4096]; snprintf(bcmd, sizeof(bcmd), "__run_shell__:%s", nc.build_cmd); st = execute_command(bcmd); }
                if (do_run_after && nc.run_cmd[0]) {
                    if (st == 0) {
                        char rc[4096]; snprintf(rc, sizeof(rc), "__run_shell__:%s", nc.run_cmd); execute_command(rc);
                    } else {
                        char emsg[256]; snprintf(emsg, sizeof(emsg), "Run skipped: build failed (exit %d)", st);
                        output_append_colored_line(emsg);
                        output_write_raw_to_log(emsg);
                        E.output_visible = 1; E.output_scroll = 0; E.output_sel = 0; editorRefreshScreen();
                    }
                }
                return;
            } else {
                snprintf(E.msg, sizeof(E.msg), "Interactive setup cancelled"); E.msg_time = time(NULL);
            }
        }
    }
}

static void handle_run_only(int allow_interactive, const char *arg) {
    // If an explicit argument was provided (e.g., "R file.py"), run it directly and return
    if (arg && arg[0]) {
        int rc = run_interactive(arg);
        if (rc == 0) return; // success
        if (rc == -1) { snprintf(E.msg, sizeof(E.msg), "Failed to run %s", arg); E.msg_time = time(NULL); return; }
        snprintf(E.msg, sizeof(E.msg), "Run finished (exit %d)", rc); E.msg_time = time(NULL); return;
    }

    char path[256] = {0}; BuildSystem bs = detect_build_system(path, sizeof(path));
    if (bs != BS_NONE) {
        // list typical run targets
        char *cmd = NULL;
        if (bs == BS_MAKE) {
            int tn=0; char **t = get_make_targets(&tn);
            // try to find 'run' target
            int found = -1; for (int i=0;i<tn;i++) if (strcmp(t[i], "run")==0) { found = i; break; }
            if (found >= 0) cmd = malloc(128), snprintf(cmd,128, "make %s", t[found]);
            free_string_array(t, tn);
        } else if (bs == BS_CARGO) {
            cmd = strdup("cargo run");
        } else if (bs == BS_CMAKE) {
            cmd = strdup("ctest --output-on-failure");
        }
        if (!cmd) { char *in = prompt_input("Enter command to run: "); if (in) { cmd = in; } }
        if (cmd) { char runcmd[768]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", cmd); execute_command(runcmd); free(cmd); return; }
    } else {
        // no build system found: check config
        BuildConfig cfg = {0};
        if (load_build_config(&cfg)) {
            if (prompt_yesno("Saved build config found. Run it?")) {
                if (cfg.run_cmd[0]) { char rc[4096]; snprintf(rc, sizeof(rc), "__run_shell__:%s", cfg.run_cmd); execute_command(rc); }
                else if (cfg.build_cmd[0]) { char bc[4096]; snprintf(bc, sizeof(bc), "__run_shell__:%s", cfg.build_cmd); execute_command(bc); }
            }
            // In all cases when a saved config is present, we won't fall through to interactive setup for run-only
            return;
        }
        // No saved config: only prompt to do interactive setup if allowed (B/BR flows). For run-only (allow_interactive==0) just return.
        if (allow_interactive && prompt_yesno("No build system found. Run interactive setup?")) {
            BuildConfig nc = {0};
            if (interactive_setup(&nc)) {
                if (nc.run_cmd[0]) { char rc[4096]; snprintf(rc, sizeof(rc), "__run_shell__:%s", nc.run_cmd); execute_command(rc); }
                else if (nc.build_cmd[0]) { char bc[4096]; snprintf(bc, sizeof(bc), "__run_shell__:%s", nc.build_cmd); execute_command(bc); }
                return;
            } else {
                snprintf(E.msg, sizeof(E.msg), "Interactive setup cancelled"); E.msg_time = time(NULL);
            }
        }
        // If we didn't run saved config and interactive setup isn't allowed (typical for R), try to run executable based on current file
        if (!allow_interactive) {
            // First attempt: try unified runner (interpreters or ./basename)
            if (E.filename && E.filename[0]) {
                int rc = run_interactive(E.filename);
                if (rc == 0) return; // success
                // If runner failed, fall back to attempting a quick single-file build+run for compiled languages

                char exe[256];
                const char *base = strrchr(E.filename, '/');
                base = base ? base + 1 : E.filename;
                char *dot = strrchr(base, '.');
                if (dot) {
                    int len = dot - base;
                    if (len > 0 && len < (int)sizeof(exe) - 3) {
                        memcpy(exe, base, len);
                        exe[len] = '\0';
                        char full[512];
                        snprintf(full, sizeof(full), "./%s", exe);

                        struct stat ssrc, sexe; int has_src = (stat(E.filename, &ssrc) == 0);
                        int has_exe = (stat(full, &sexe) == 0);

                        // Helper to attempt simple single-file build based on extension
                        char buildcmd[1024] = {0};
                        const char *ext = dot;
                        if (ext) {
                            if (strcmp(ext, ".c") == 0) snprintf(buildcmd, sizeof(buildcmd), "gcc -o %s %s", exe, E.filename);
                            else if (strcmp(ext, ".cpp") == 0 || strcmp(ext, ".cc") == 0) snprintf(buildcmd, sizeof(buildcmd), "g++ -o %s %s", exe, E.filename);
                            else if (strcmp(ext, ".rs") == 0) snprintf(buildcmd, sizeof(buildcmd), "rustc -o %s %s", exe, E.filename);
                            else if (strcmp(ext, ".go") == 0) snprintf(buildcmd, sizeof(buildcmd), "go build -o %s %s", exe, E.filename);
                            else if (strcmp(ext, ".py") == 0) buildcmd[0] = '\0';
                            else if (strcmp(ext, ".java") == 0) snprintf(buildcmd, sizeof(buildcmd), "javac %s && java %s", E.filename, exe);
                        }

                        // If executable exists and is newer than source, just run it
                        if (has_exe && has_src && difftime(sexe.st_mtime, ssrc.st_mtime) >= 0) {
                            char runcmd[768]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", full);
                            execute_command(runcmd); return;
                        }

                        // If executable missing but we have a sensible build command, try to build
                        if ((!has_exe || (has_src && has_exe && difftime(sexe.st_mtime, ssrc.st_mtime) < 0)) && buildcmd[0]) {
                            char bcmd[4096]; snprintf(bcmd, sizeof(bcmd), "__run_shell__:%s", buildcmd);
                            int st = execute_command(bcmd);
                            if (st == 0) {
                                // build succeeded; run
                                char runcmd[768]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", full);
                                execute_command(runcmd); return;
                            } else {
                                // build failed; do not run old binary
                                snprintf(E.msg, sizeof(E.msg), "Build failed; run cancelled"); E.msg_time = time(NULL);
                                return;
                            }
                        }

                        // If no buildcmd available but executable exists, run it (best effort)
                        if (has_exe) { char runcmd[768]; snprintf(runcmd, sizeof(runcmd), "__run_shell__:%s", full); execute_command(runcmd); return; }
                    }
                }
            }
            snprintf(E.msg, sizeof(E.msg), "No build config found; run cancelled"); E.msg_time = time(NULL);
        }
    }
}

static void show_file_browser(void) {
    char **entries = NULL; int nentries = 0;
    char cwd[512]; getcwd(cwd, sizeof(cwd));
    // remember whether we started the browser from the welcome page; use this
    // to decide whether to restore the welcome page if the user cancels.
    int started_from_welcome = (E.welcome_visible || E.browser_from_welcome);
    /* clear browser_from_welcome to avoid stale state in later calls */
    E.browser_from_welcome = 0;
    // ensure other overlays (help) are hidden while browser is active
    E.help_visible = 0;
    int file_opened = 0;

    /* directory history stack: dir_history[0] is the initial directory, top is dir_history[dh_count-1]
     * When user enters a directory we push the new cwd; Backspace pops and returns to previous cwd.
     */
    char **dir_history = NULL; int dh_count = 0;
    // push initial cwd
    dir_history = realloc(dir_history, sizeof(char*) * (dh_count + 1)); if (dir_history) dir_history[dh_count++] = strdup(cwd);

    // helper to (re)populate entries for current directory
    auto_reload:
    for (int i = 0; i < nentries; i++) { free(entries[i]); }
    free(entries); entries = NULL; nentries = 0;
    DIR *d = opendir(".");
    if (!d) { snprintf(E.msg, sizeof(E.msg), "Cannot open dir: %s", strerror(errno)); E.msg_time = time(NULL); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0) continue;
        // include ".." to go up
        char *name = NULL;
        struct stat st;
        if (stat(de->d_name, &st) == 0 && S_ISDIR(st.st_mode)) {
            int l = strlen(de->d_name) + 2; name = malloc(l); snprintf(name, l, "%s/", de->d_name);
        } else {
            name = strdup(de->d_name);
        }
        if (!name) continue;
        char **n = realloc(entries, sizeof(char*) * (nentries + 1)); if (!n) { free(name); continue; }
        entries = n; entries[nentries++] = name;
    }
    closedir(d);
    if (nentries == 0) { snprintf(E.msg, sizeof(E.msg), "(empty)"); E.msg_time = time(NULL); return; }
    qsort(entries, nentries, sizeof(char*), alphasort_entries);

    int page = 0;
    char selbuf[32] = {0}; int selpos = 0;
    while (1) {
        /* if terminal was resized, update dimensions and recompute page size */
        if (winch_received) { getWindowSize(); editorScroll(); editorRefreshScreen(); winch_received = 0; }
        int page_size = E.screenrows - 8; if (page_size < 3) page_size = 3;

        // clear underlying editor region (clear full screen to avoid leftover artifacts from editor content)
        for (int _ln = 1; _ln <= E.screenrows; _ln++) {
            char __tmp[64]; int __n = snprintf(__tmp, sizeof(__tmp), "\x1b[%d;1H\x1b[2K", _ln);
            if (__n > 0) write(STDOUT_FILENO, __tmp, (size_t)__n);
        }
        // draw overlay box
        int total_pages = (nentries + page_size - 1) / page_size;
        if (page >= total_pages) page = total_pages - 1;
        int start = page * page_size; int end = start + page_size; if (end > nentries) end = nentries;
        // compute maximum entry width
        int maxn = 0;
        for (int i = start; i < end; i++) { int l = (int)strlen(entries[i]); if (l > maxn) maxn = l; }
        // number of entries on this page affects number column width
        int entries_on_page = end - start;
        int numDigits = (entries_on_page > 0) ? snprintf(NULL, 0, "%d", entries_on_page) : 1;
        int local_prefix = numDigits + 3; // e.g. "NN) "
        // compute box width/height ensuring it can contain the widest entry and prompt
        int minBoxW = local_prefix + maxn + 4;
        int boxw = minBoxW;
        /*
         * If there's more horizontal space available than the minimum required,
         * prefer to use it so long paths/help text aren't unnecessarily
         * truncated. This makes the overlay expand when the terminal is
         * enlarged.
         */
        if (E.screencols > boxw) boxw = E.screencols;
        if (boxw > E.screencols) boxw = E.screencols; // still clamp as a safety
        // add an extra line so we can render two prompt lines (selection + help)
        int boxh = (end - start) + 5;
        if (boxh > E.screenrows - 4) boxh = E.screenrows - 4;
        int max_base = boxw - 3 - 1;
        if (max_base < 0) max_base = 0;
        // help text for second line: use the explicit parenthetical labels the user requested
        char promptBase[128];
        const char *fullPrompt = "\x1b[93mPgUp/PgDn\x1b[0m (page), \x1b[93mBackspace\x1b[0m (Back), \x1b[93mq/ESC\x1b[0m (cancel)";
        const char *shortPrompt = "\x1b[93mPgUp/Dn\x1b[0m (pg), \x1b[93mBS\x1b[0m (bk), \x1b[93mq\x1b[0m (can)";
        // helper to calculate visible length (ignoring ANSI escape codes)
        int visible_len(const char *s) {
            int len = 0;
            while (*s) {
                if (*s == '\x1b') {
                    while (*s && *s != 'm') s++;
                    if (*s) s++;
                } else {
                    len++;
                    s++;
                }
            }
            return len;
        }
        // helper to find string index for given visible length
        int visible_index(const char *s, int vis) {
            int len = 0;
            int idx = 0;
            while (*s && len < vis) {
                if (*s == '\x1b') {
                    while (*s && *s != 'm') { s++; idx++; }
                    if (*s) { s++; idx++; }
                } else {
                    len++;
                    s++;
                    idx++;
                }
            }
            return idx;
        }
        int fullVisible = visible_len(fullPrompt);
        int useShort = (fullVisible > max_base) ? 1 : 0;
        const char *chosenPrompt = useShort ? shortPrompt : fullPrompt;
        int chosenVisible = visible_len(chosenPrompt);
        int trunc_visible = (chosenVisible > max_base) ? max_base : chosenVisible;
        int trunc_index = visible_index(chosenPrompt, trunc_visible);
        snprintf(promptBase, sizeof(promptBase), "%.*s", trunc_index, chosenPrompt);
        int sx = 1; int sy = (E.screenrows - boxh) / 2; // left-align overlay so it covers editor gutter
        // background -- clear the whole box region to avoid leftover artifacts
        for (int y = 0; y < boxh; y++) {
            char buf[128]; int ln = sy + y + 1;
            int written = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", ln, sx + 1);
            if (written > 0) write(STDOUT_FILENO, buf, (size_t)written);
            for (int x = 0; x < boxw; x++) write(STDOUT_FILENO, " ", 1);
        }
        // title (truncate if needed to fit)
        {
            char title[256]; int tn = snprintf(title, sizeof(title), "Files in %s (page %d/%d)", cwd, page+1, total_pages);
            if (tn > boxw - 6) title[boxw - 6] = '\0';
            char buf[256]; int ln = sy + 1; int col = sx + 3; int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH%s", ln, col, title);
            if (n>0) write(STDOUT_FILENO, buf, (size_t)n);
            // clear remaining title line to avoid leftover chars from previous frames
            int title_len = (int)strlen(title);
            int remaining = boxw - (col - sx) - title_len - 1; if (remaining > 0) { for (int _i = 0; _i < remaining; _i++) write(STDOUT_FILENO, " ", 1); }
        }
        // list entries (number column is aligned, and entry names are truncated to fit)
        for (int i = start; i < end; i++) {
            int idx = i - start; int ln = sy + 2 + idx; int col = sx + 3;
            char numbuf[64]; int written = snprintf(numbuf, sizeof(numbuf), "\x1b[%d;%dH%*d) ", ln, col, numDigits, idx + 1);
            if (written > 0) write(STDOUT_FILENO, numbuf, (size_t)written);
            int nameCols = boxw - (col - sx) - (numDigits + 3);
            if (nameCols < 1) nameCols = 1;
            int is_dir = (entries[i][strlen(entries[i]) - 1] == '/');
            // color directories green and files yellow
            int lnwrite = (int)strlen(entries[i]); if (lnwrite > nameCols) lnwrite = nameCols;
            if (is_dir) write(STDOUT_FILENO, "\x1b[32m", 5);
            else write(STDOUT_FILENO, "\x1b[93m", 5);
            write(STDOUT_FILENO, entries[i], (size_t)lnwrite);
            // pad with spaces if the new name is shorter than the previous content
            int pad = nameCols - lnwrite; if (pad > 0) { for (int _p = 0; _p < pad; _p++) write(STDOUT_FILENO, " ", 1); }
            write(STDOUT_FILENO, "\x1b[0m", 4);
        }
        // prompt: two lines
        // 1) selection line (label + current selection)
        {
            int ln = sy + boxh - 2; int col = sx + 3;
            const char sel_label[] = "Select number: ";
            // write label in red
            char pbuf[256]; int n = snprintf(pbuf, sizeof(pbuf), "\x1b[%d;%dH\x1b[93m%s\x1b[0m", ln, col, sel_label);
            if (n>0) write(STDOUT_FILENO, pbuf, (size_t)n);
            // write selection (uncolored so typed digits are clear)
            int write_col = col + (int)strlen(sel_label);
            char sbuf[128]; int sn = snprintf(sbuf, sizeof(sbuf), "\x1b[%d;%dH%s", ln, write_col, selbuf);
            if (sn>0) write(STDOUT_FILENO, sbuf, (size_t)sn);
        }
        // 2) help line (PgUp/PgDn, Backspace behavior, q to cancel)
        {
            int ln = sy + boxh - 1; int col = sx + 3;
            char pbuf2[512]; int n2 = snprintf(pbuf2, sizeof(pbuf2), "\x1b[%d;%dH%s", ln, col, promptBase);
            if (n2>0) write(STDOUT_FILENO, pbuf2, (size_t)n2);
        }
        // move cursor to selection input area (on selection line)
        char cur[64]; int crow = sy + boxh - 2;
        int sel_label_len = (int)strlen("Select number: ");
        int curcol = sx + 3 + sel_label_len + (int)strlen(selbuf);
        if (curcol > sx + boxw - 2) curcol = sx + boxw - 2;
        int m = snprintf(cur, sizeof(cur), "\x1b[%d;%dH", crow, curcol);
        if (m>0) write(STDOUT_FILENO, cur, (size_t)m);

        // read keys
        int k = readKey(); if (k == -1) { continue; }
        if (k == 27 || k == 'q' || k == 'Q') { snprintf(E.msg, sizeof(E.msg), "Browser cancelled"); E.msg_time = time(NULL); break; }
        if (k == PAGE_DOWN) { if (page + 1 < total_pages) page++; selbuf[0] = '\0'; selpos = 0; continue; }
        if (k == PAGE_UP) { if (page > 0) page--; selbuf[0] = '\0'; selpos = 0; continue; }
        // Backspace now only deletes typed selection (if any); if nothing typed, behaves like go-back/cancel
        if (k == 127 || k == 8) {
            /* If there is a typed selection, delete a digit first (expected behavior).
             * Otherwise, if we have a directory history, go back. If we're at the
             * initial directory, do NOT immediately cancel on a single Backspace to
             * avoid accidental closure; instead show a hint (press q to cancel).
             */
            if (selpos > 0) { selpos--; selbuf[selpos] = '\0'; continue; }
            if (dh_count > 1) {
                // pop current
                free(dir_history[dh_count - 1]); dir_history[dh_count - 1] = NULL; dh_count--;
                // chdir to previous
                if (chdir(dir_history[dh_count - 1]) == 0) {
                    getcwd(cwd, sizeof(cwd));
                    // reset pagination and selection
                    page = 0; selbuf[0] = '\0'; selpos = 0;
                    goto auto_reload;
                } else {
                    snprintf(E.msg, sizeof(E.msg), "Chdir failed: %s", strerror(errno)); E.msg_time = time(NULL);
                    selbuf[0] = '\0'; selpos = 0; continue;
                }
            } else {
                /* At initial directory and no digits typed: show a hint instead of
                 * cancelling immediately (prevents accidental closure).
                 */
                snprintf(E.msg, sizeof(E.msg), "Press q or Esc to cancel the browser"); E.msg_time = time(NULL);
                continue;
            }
        }
        if (k >= '0' && k <= '9') { if (selpos < (int)sizeof(selbuf)-2) { selbuf[selpos++] = (char)k; selbuf[selpos] = '\0'; } continue; }
        if (k == '\r' || k == '\n') {
            if (selpos == 0) continue;
            int sel = atoi(selbuf); if (sel <= 0 || sel > (end - start)) { snprintf(E.msg, sizeof(E.msg), "Bad selection"); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue; }
            int selidx = start + sel - 1;
            // if directory, chdir and reload
            int is_dir = (entries[selidx][strlen(entries[selidx]) - 1] == '/');
            if (is_dir) {
                // directory selected: offer Open / Delete / Cancel, except for ".." which just goes up
                char tmp[512]; snprintf(tmp, sizeof(tmp), "%s", entries[selidx]); tmp[strlen(tmp)-1] = '\0';
                if (strcmp(tmp, "..") == 0) {
                    // special case: ".." just goes up without prompt
                    if (chdir(tmp) == 0) {
                        getcwd(cwd, sizeof(cwd));
                        char **nh = realloc(dir_history, sizeof(char*) * (dh_count + 1));
                        if (nh) { dir_history = nh; dir_history[dh_count++] = strdup(cwd); }
                        page = 0; selbuf[0] = '\0'; selpos = 0;
                        goto auto_reload;
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "Chdir failed: %s", strerror(errno)); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue;
                    }
                }
                char __tmpbuf[128]; int __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                int pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "Directory '%s': \x1b[92m(o) Open\x1b[0m / \x1b[91m(d) Delete\x1b[0m / \x1b[94m(g) Change Directory\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", tmp);
                if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                int dresp = 0;
                while (1) {
                    int kk = readKey(); if (kk == -1) continue;
                    if (kk == 'o' || kk == 'O' || kk == '\r' || kk == '\n') { dresp = 'o'; break; }
                    if (kk == 'd' || kk == 'D') { dresp = 'd'; break; }
                    if (kk == 'g' || kk == 'G') { dresp = 'g'; break; }
                    if (kk == 'c' || kk == 'C' || kk == 27) { dresp = 'c'; break; }
                }
                __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                if (dresp == 'c') { snprintf(E.msg, sizeof(E.msg), "Browse cancelled"); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue; }
                if (dresp == 'o') {
                    if (chdir(tmp) == 0) {
                        getcwd(cwd, sizeof(cwd));
                        char **nh = realloc(dir_history, sizeof(char*) * (dh_count + 1));
                        if (nh) { dir_history = nh; dir_history[dh_count++] = strdup(cwd); }
                        page = 0; selbuf[0] = '\0'; selpos = 0;
                        goto auto_reload;
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "Chdir failed: %s", strerror(errno)); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue;
                    }
                }
                if (dresp == 'g') {
                    if (chdir(tmp) == 0) {
                        char newcwd[512]; getcwd(newcwd, sizeof(newcwd));
                        snprintf(E.msg, sizeof(E.msg), "Changed directory to %s", newcwd);
                        E.msg_time = time(NULL);
                        break; // exit browser
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "Chdir failed: %s", strerror(errno)); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue;
                    }
                }
                if (dresp == 'd') {
                    // confirm delete
                    __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[93mDelete directory '%s'?\x1b[0m \x1b[92m(y)\x1b[0m\x1b[91m(N)\x1b[0m: ", tmp);
                    if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                    int y = 0; while (1) { int k2 = readKey(); if (k2 == -1) continue; if (k2=='y') { y = 1; break; } if (k2=='N' || k2==27) { y = 0; break; } }
                    __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                    if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                    if (!y) { snprintf(E.msg, sizeof(E.msg), "Delete cancelled"); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue; }
                    if (rmdir(tmp) == 0) {
                        snprintf(E.msg, sizeof(E.msg), "Deleted directory %s", tmp);
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "rmdir failed: %s", strerror(errno));
                    }
                    E.msg_time = time(NULL);
                    selbuf[0] = '\0'; selpos = 0; goto auto_reload;
                }
            } else {
                // prompt user whether to open in current window or in a new tab
                char __tmpbuf[256]; int __tmpn;
                /* Clear the last one or two lines before drawing the prompt */
                if (E.screenrows > 1) __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K\x1b[%d;1H\x1b[K", E.screenrows - 1, E.screenrows);
                else __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                // determine whether current buffer is unsaved
                int cur_unsaved = 0; char *curss_tmp = serialize_buffer(); if (curss_tmp) { if (!(E.saved_snapshot && strcmp(E.saved_snapshot, curss_tmp) == 0)) cur_unsaved = 1; free(curss_tmp); }
                int pn;
                /* Show the concise initial choices; defer save/discard confirmation until the user selects Current Tab. Include (d) Delete. */
                if (cur_unsaved) pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mOpen\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mas:\x1b[0m \x1b[94m(t) New Tab\x1b[0m / \x1b[92m(o) Current Tab\x1b[0m / \x1b[91m(d) Delete\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", entries[selidx]);
                else pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[1;97mOpen\x1b[0m \x1b[96m'%s'\x1b[0m \x1b[1;97mas:\x1b[0m \x1b[94m(t) New Tab\x1b[0m / \x1b[92m(o) Current Tab\x1b[0m / \x1b[91m(d) Delete\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", entries[selidx]);
                if (pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)pn);
                int resp = 0;
                while (1) {
                    int kk = readKey(); if (kk == -1) continue;
                    if (kk == 't' || kk == 'T' || kk == 'n' || kk == 'N') { resp = 't'; break; }
                    if (kk == 'o' || kk == 'O' || kk == '\r' || kk == '\n') { resp = 'o'; break; }
                    if (kk == 'd' || kk == 'D') { resp = 'd'; break; }
                    if (kk == 'c' || kk == 'C' || kk == 27) { resp = 'c'; break; }
                }
                // clear prompt line
                __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                if (resp == 'c') { snprintf(E.msg, sizeof(E.msg), "Open cancelled"); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue; }
                if (resp == 'd') {
                    // confirm delete
                    char __tmp2[128]; int __n2 = snprintf(__tmp2, sizeof(__tmp2), "\x1b[%d;1H\x1b[K\x1b[93mDelete '%s'?\x1b[0m \x1b[92m(y)\x1b[0m\x1b[91m(N)\x1b[0m: ", E.screenrows, entries[selidx]);
                    if (__n2 > 0) write(STDOUT_FILENO, __tmp2, (size_t)__n2);
                    int ok = 0; while (1) { int k2 = readKey(); if (k2 == -1) continue; if (k2=='y') { ok = 1; break; } if (k2=='N' || k2==27) { ok = 0; break; } }
                    __n2 = snprintf(__tmp2, sizeof(__tmp2), "\x1b[%d;1H\x1b[K", E.screenrows); if (__n2>0) write(STDOUT_FILENO, __tmp2, (size_t)__n2);
                    if (!ok) { snprintf(E.msg, sizeof(E.msg), "Delete cancelled"); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue; }
                    // perform unlink
                    if (access(entries[selidx], F_OK) == 0) {
                        if (unlink(entries[selidx]) == 0) {
                            // if we deleted the currently open file, switch similar to fn del
                            if (E.filename && strcmp(E.filename, entries[selidx]) == 0) {
                                if (NumTabs > 0) {
                                    int target = NumTabs - 1;
                                    if (switch_to_tab(target) == 0) {
                                        snprintf(E.msg, sizeof(E.msg), "Deleted %s; switched to tab %d", entries[selidx], target+2);
                                    } else {
                                        free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); E.welcome_visible = 1; snprintf(E.msg, sizeof(E.msg), "Deleted %s; opened empty buffer", entries[selidx]);
                                    }
                                } else {
                                    free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer(); snprintf(E.msg, sizeof(E.msg), "Deleted %s; opened empty buffer", entries[selidx]);
                                }
                            } else {
                                snprintf(E.msg, sizeof(E.msg), "Deleted %s", entries[selidx]);
                            }
                        } else {
                            snprintf(E.msg, sizeof(E.msg), "Delete failed: %s", strerror(errno));
                        }
                    } else {
                        snprintf(E.msg, sizeof(E.msg), "No such file %s", entries[selidx]);
                    }
                    E.msg_time = time(NULL);
                    selbuf[0] = '\0'; selpos = 0; goto auto_reload;
                }
                if (resp == 't') { file_opened = 1; open_file_in_new_tab(entries[selidx]); break; }
                if (resp == 's') { save_to_file(NULL); open_file_in_current_window_no_push(entries[selidx]); break; }
                // resp == 'o' -> open in current window; if unsaved, this will discard unsaved changes
                if (resp == 'o') {
                    if (cur_unsaved) {
                        /* Ask for confirmation and offer save option to avoid accidental data loss */
                        int __pn;
                        if (E.screenrows > 3) {
                            /* four-line layout: message + three option lines */
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K\x1b[1mCurrent file has unsaved changes:\x1b[0m", E.screenrows - 3);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K(s) \x1b[32mSave changes and open file\x1b[0m", E.screenrows - 2);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K(d) \x1b[91mDiscard unsaved changes and open file\x1b[0m", E.screenrows - 1);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K(c) \x1b[93mCancel\x1b[0m", E.screenrows);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                        } else if (E.screenrows > 1) {
                            /* two-line fallback */
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K\x1b[1mCurrent file has unsaved changes:\x1b[0m", E.screenrows - 1);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K(s) \x1b[32mSave changes and open file\x1b[0m / (d) \x1b[91mDiscard unsaved changes and open file\x1b[0m / (c) \x1b[93mCancel\x1b[0m", E.screenrows);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                        } else {
                            /* single-line fallback */
                            __pn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K\x1b[93mCurrent file has unsaved changes.\x1b[0m \x1b[32m(s) Save changes and open file\x1b[0m / \x1b[91m(d) Discard unsaved changes and open file\x1b[0m / \x1b[93m(c) Cancel\x1b[0m: ", E.screenrows);
                            if (__pn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__pn);
                        }
                        int __r = 0;
                        while (1) {
                            int __kk = readKey(); if (__kk == -1) continue;
                            if (__kk == 's' || __kk == 'S') { __r = 's'; break; }
                            if (__kk == 'd' || __kk == 'D' || __kk == '\r' || __kk == '\n') { __r = 'd'; break; }
                            if (__kk == 'c' || __kk == 'C' || __kk == 27) { __r = 'c'; break; }
                        }
                        if (E.screenrows > 3) __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K\x1b[%d;1H\x1b[K\x1b[%d;1H\x1b[K\x1b[%d;1H\x1b[K", E.screenrows - 3, E.screenrows - 2, E.screenrows - 1, E.screenrows);
                        else if (E.screenrows > 1) __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K\x1b[%d;1H\x1b[K", E.screenrows - 1, E.screenrows);
                        else __tmpn = snprintf(__tmpbuf, sizeof(__tmpbuf), "\x1b[%d;1H\x1b[K", E.screenrows);
                        if (__tmpn > 0) write(STDOUT_FILENO, __tmpbuf, (size_t)__tmpn);
                        if (__r == 'c') { snprintf(E.msg, sizeof(E.msg), "Open cancelled"); E.msg_time = time(NULL); selbuf[0] = '\0'; selpos = 0; continue; }
                        if (__r == 's') { save_to_file(NULL); file_opened = 1; open_file_in_current_window_no_push(entries[selidx]); break; }
                        /* fallthrough for confirmed discard */
                    }
                    open_file_in_current_window_no_push(entries[selidx]);
                    file_opened = 1;
                    break;
                }
                break;
            }
        }
        // any other key resets selection buffer
        selbuf[0] = '\0'; selpos = 0;
    }
    for (int i = 0; i < nentries; i++) { free(entries[i]); }
    free(entries); entries = NULL; nentries = 0;
    // free directory history
    if (dir_history) {
        for (int i = 0; i < dh_count; i++) { free(dir_history[i]); }
        free(dir_history); dir_history = NULL; dh_count = 0;
    }
    if (started_from_welcome && !file_opened) {
        E.welcome_visible = 1;
        E.browser_from_welcome = 0;
    }
    // redraw editor
    editorRefreshScreen();
}


static void editorRefreshScreen(void) {
    // clear and redraw
    write(STDOUT_FILENO, "\x1b[?25l", 6); // hide cursor
    write(STDOUT_FILENO, "\x1b[H", 3); // move to top-left

    // If welcome page is visible, render it as a true full page (no status/tab/bar or buffer)
    if (E.welcome_visible) {
        write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
        write(STDOUT_FILENO, "\x1b[H", 3);  // move to top-left
        draw_welcome_overlay();
        // If suggestions are visible while on the welcome page, render them on top
        if (SuggestionsVisible) { render_suggestions_overlay(); return; }
        return;
    }

    // Status bar (fixed top row)
    int content_rows = E.screenrows - 1; // reserve status
    if (content_rows < 1) content_rows = 1;

    // Optionally render a tab bar below the status row
    if (conf.show_tab_bar) {
        // build and render a simple tab bar: fixed order [1]home [2]stored0 [3]stored1...
        // format: [1]name[*] [2]name[*] ... where the active entry is reverse-video
        int remaining = E.screencols;
        // determine home name and unsaved
        const char *home_name;
        int home_unsaved = 0;
        if (CurTab == -1) {
            home_name = (E.filename && *E.filename) ? E.filename : "(No Name)";
            char *curss = serialize_buffer();
            if (curss) {
                if (!(E.saved_snapshot && strcmp(E.saved_snapshot, curss) == 0)) home_unsaved = 1;
                free(curss);
            }
        } else {
            home_name = (HaveHomeTab && HomeTab.name && *HomeTab.name) ? HomeTab.name : "(No Name)";
            if (HomeTab.saved_snapshot && HomeTab.snapshot) {
                home_unsaved = (strcmp(HomeTab.saved_snapshot, HomeTab.snapshot) != 0);
            } else if (HomeTab.saved_snapshot == NULL && HomeTab.snapshot) {
                home_unsaved = 1;
            }
        }
        int tab_index = 1;
        // render home (display-friendly)
        {
            char *home_display = normalize_display_path(home_name);
            int namelen = (int)strlen(home_display ? home_display : home_name);
            int bracket_len = 4 + (home_unsaved ? 1 : 0); // [1] or [*1]
            int blen = bracket_len + namelen + 1; // + space
            if (blen > remaining) {
                char pfx[32]; int pn = home_unsaved ? snprintf(pfx, sizeof(pfx), "[*1]") : snprintf(pfx, sizeof(pfx), "[1]");
                if (CurTab == -1) write(STDOUT_FILENO, "\x1b[7m", 4);
                if (pn > remaining) write(STDOUT_FILENO, pfx, remaining);
                else {
                    write(STDOUT_FILENO, pfx, pn);
                    int can = remaining - pn;
                    if (can > 0) write(STDOUT_FILENO, home_display ? home_display : "", can);
                }
                if (CurTab == -1) write(STDOUT_FILENO, "\x1b[m", 3);
                remaining = 0;
            } else {
                if (CurTab == -1) write(STDOUT_FILENO, "\x1b[7m", 4);
                write(STDOUT_FILENO, "[", 1);
                if (home_unsaved) write(STDOUT_FILENO, "\x1b[91m*\x1b[0m", 8);
                write(STDOUT_FILENO, "\x1b[93m", 5);
                write(STDOUT_FILENO, "1", 1);
                write(STDOUT_FILENO, "\x1b[0m", 4);
                write(STDOUT_FILENO, "]", 1);
                write(STDOUT_FILENO, home_display ? home_display : "", namelen);
                write(STDOUT_FILENO, " ", 1);
                if (CurTab == -1) write(STDOUT_FILENO, "\x1b[m", 3);
                remaining -= blen;
            }
            tab_index++; if (home_display) free(home_display);
        }
        // render stored tabs
        for (int t = 0; t < NumTabs && remaining > 0; t++) {
            const char *tname = (Tabs[t].name && *Tabs[t].name) ? Tabs[t].name : "(No Name)";
            char *t_display = normalize_display_path(tname);
            int namelen = (int)strlen(t_display ? t_display : tname);
            int unsaved = 0;
            if (t == CurTab) {
                char *curss = serialize_buffer();
                if (curss) {
                    if (!(E.saved_snapshot && strcmp(E.saved_snapshot, curss) == 0)) unsaved = 1;
                    free(curss);
                } else {
                    unsaved = 1;
                }
            } else {
                if (Tabs[t].saved_snapshot && Tabs[t].snapshot) {
                    unsaved = (strcmp(Tabs[t].saved_snapshot, Tabs[t].snapshot) != 0);
                } else if (Tabs[t].saved_snapshot == NULL && Tabs[t].snapshot) {
                    unsaved = 1;
                }
            }
            int num_digits = snprintf(NULL, 0, "%d", tab_index);
            int bracket_len = 2 + num_digits + 2 + (unsaved ? 1 : 0); // [ N ] or [ * N ]
            int blen = bracket_len + namelen + 1;
            if (blen > remaining) {
                char pfx[32]; int pn = unsaved ? snprintf(pfx, sizeof(pfx), "[*%d]", tab_index) : snprintf(pfx, sizeof(pfx), "[%d]", tab_index);
                if (t == CurTab) write(STDOUT_FILENO, "\x1b[7m", 4);
                if (pn > remaining) write(STDOUT_FILENO, pfx, remaining);
                else {
                    write(STDOUT_FILENO, pfx, pn);
                    int can = remaining - pn;
                    if (can > 0) write(STDOUT_FILENO, t_display ? t_display : "", can);
                }
                if (t == CurTab) write(STDOUT_FILENO, "\x1b[m", 3);
                remaining = 0;
                break;
            }
            if (t == CurTab) write(STDOUT_FILENO, "\x1b[7m", 4);
            write(STDOUT_FILENO, "[", 1);
            if (unsaved) write(STDOUT_FILENO, "\x1b[91m*\x1b[0m", 8);
            char numbuf[16]; int nn = snprintf(numbuf, sizeof(numbuf), "%d", tab_index);
            write(STDOUT_FILENO, "\x1b[93m", 5);
            write(STDOUT_FILENO, numbuf, nn);
            write(STDOUT_FILENO, "\x1b[0m", 4);
            write(STDOUT_FILENO, "]", 1);
            write(STDOUT_FILENO, t_display ? t_display : "", namelen);
            write(STDOUT_FILENO, " ", 1);
            if (t == CurTab) write(STDOUT_FILENO, "\x1b[m", 3);
            remaining -= blen;
            tab_index++;
            if (t_display) free(t_display);
        }
        // fill rest of line with spaces and newline
        for (int i = 0; i < remaining; i++) write(STDOUT_FILENO, " ", 1);
        write(STDOUT_FILENO, "\r\n", 2);
        // reduce content rows available by 1
        content_rows = E.screenrows - 2; if (content_rows < 1) content_rows = 1;
    }
    char status[256];
    /* show filename (or No Name) and saved/unsaved marker */
    const char *name = E.filename ? E.filename : "No Name";
    /* display-friendly name (strip leading './' if present) */
    char *display_name = NULL; if (name && name[0]) display_name = normalize_display_path(name);
    char save_mark[16] = "";
    if (!E.saved_snapshot) {
        snprintf(save_mark, sizeof(save_mark), " (unsaved)");
    } else {
        char *curss = serialize_buffer();
        if (curss) {
            if (strcmp(curss, E.saved_snapshot) == 0) snprintf(save_mark, sizeof(save_mark), " (saved)");
            else snprintf(save_mark, sizeof(save_mark), " (unsaved)");
            free(curss);
        } else {
            snprintf(save_mark, sizeof(save_mark), " (unsaved)");
        }
    }
    int nleft = snprintf(status, sizeof(status), "%s%s - %d lines", display_name ? display_name : name, save_mark, E.numrows);
    if (display_name) { free(display_name); display_name = NULL; }
    char rstatus[64];
    int visible = E.numrows - E.row_offset;
    if (visible > content_rows) visible = content_rows;
    if (visible < 0) visible = 0;
    int nr = snprintf(rstatus, sizeof(rstatus), "Ln %d, Col %d", E.cy + 1, E.cx + 1);
    /* if there's a recent transient message, show it on the right side for visibility */
    time_t now = time(NULL);
    if (E.msg_time && now - E.msg_time < 5) {
        nr = snprintf(rstatus, sizeof(rstatus), "%s", E.msg);
    }
    // compose status with padding to full width
    int total = E.screencols;
    if (nleft + nr >= total) {
        // truncate left if needed
        status[total - nr - 1] = '\0';
        nleft = (int)strlen(status);
    }
    // build mode text and optionally command buffer (displayed next to mode in view)
    const char *mode_base = (E.mode == MODE_VIEW) ? "VIEW MODE" : "EDIT MODE";
    char mode_text[2048];
    int mode_len = 0;
    if (E.mode == MODE_VIEW && E.command_len > 0) {
        // display command next to mode, prefixed with ':'
        snprintf(mode_text, sizeof(mode_text), "%s :%s", mode_base, E.command_buf);
    } else {
        snprintf(mode_text, sizeof(mode_text), "%s", mode_base);
    }
    mode_len = (int)strlen(mode_text);

    // available space between left and right
    int available = total - nleft - nr;
    if (available < 0) available = 0;

    int left_pad = 0, right_pad = 0;
    if (available > mode_len) {
        int extra = available - mode_len;
        left_pad = extra / 2;
        right_pad = extra - left_pad;
    } else {
        // not enough space to show full mode text; truncate
        mode_len = available;
    }

    write(STDOUT_FILENO, "\x1b[7m", 4);
    // left part
    write(STDOUT_FILENO, status, nleft);
    // left padding
    for (int i = 0; i < left_pad; i++) write(STDOUT_FILENO, " ", 1);
    // mode text (possibly truncated). If view mode with command, color the command portion.
    if (E.mode == MODE_VIEW && E.command_len > 0) {
        // find ':' position
        char *colpos = strchr(mode_text, ':');
        if (colpos != NULL) {
            int head_len = (int)(colpos - mode_text);
            if (head_len > mode_len) head_len = mode_len;
            // write mode base
            if (head_len > 0) write(STDOUT_FILENO, mode_text, head_len);
            // write command in reverse video + yellow (inside reverse video it's still visible)
            int cmd_len = mode_len - head_len;
            if (cmd_len > 0) {
                // color with bright ANSI yellow while still reverse-video
                write(STDOUT_FILENO, "\x1b[93m", 5);
                write(STDOUT_FILENO, colpos, cmd_len);
                write(STDOUT_FILENO, "\x1b[0m", 4);
            }
        } else {
            write(STDOUT_FILENO, mode_text, mode_len);
        }
    } else {
        if (mode_len > 0) write(STDOUT_FILENO, mode_text, mode_len);
    }

    // ensure reverse-video for right padding and right part
    write(STDOUT_FILENO, "\x1b[7m", 4);
    // right padding
    for (int i = 0; i < right_pad; i++) write(STDOUT_FILENO, " ", 1);
    // right part
    write(STDOUT_FILENO, rstatus, nr);
    write(STDOUT_FILENO, "\x1b[m", 3);
    write(STDOUT_FILENO, "\r\n", 2);


    // compute width for line numbers (based on visible content rows)
    int total_lines = (E.numrows > content_rows) ? E.numrows : content_rows;
    int ln_width = snprintf(NULL, 0, "%d", total_lines);
    if (ln_width < 1) ln_width = 1;
    int prefix_len = ln_width + 3; // "%*d | " -> number + space + '|' + space

    // make sure row_offset is valid
    if (E.row_offset < 0) E.row_offset = 0;
    if (E.row_offset > E.numrows) E.row_offset = E.numrows;

    for (int y = 0; y < content_rows; y++) {
        int filerow = y + E.row_offset;
        char linebuf[64];
        if (filerow < E.numrows) {
            // print line number prefix for existing rows
            int n = snprintf(linebuf, sizeof(linebuf), "%*d | ", ln_width, filerow + 1);
            if (n > 0) write(STDOUT_FILENO, linebuf, (size_t)n);

            int avail = E.screencols - prefix_len;
            if (avail < 0) avail = 0;
            // respect horizontal offset
            int rlen = (int)strlen(E.rows[filerow]);
            int start = (E.col_offset < rlen) ? E.col_offset : rlen;
            int len = rlen - start;
            if (len > avail) len = avail;
            if (len > 0) {
                /* Render visible substring with syntax highlighting if available. */
                /* Make a temporary NUL-terminated buffer for the visible slice. */
                char tmpbuf[1024];
                char *render_ptr = NULL;
                if (len < (int)sizeof(tmpbuf)) {
                    memcpy(tmpbuf, E.rows[filerow] + start, len);
                    tmpbuf[len] = '\0';
                    render_ptr = tmpbuf;
                } else {
                    /* allocate for large lines */
                    render_ptr = malloc((size_t)len + 1);
                    if (!render_ptr) render_ptr = E.rows[filerow] + start; /* fallback */
                    else { memcpy(render_ptr, E.rows[filerow] + start, len); render_ptr[len] = '\0'; }
                }
                if (render_ptr) {
                    /* colorize_line is safe to call even if no language is set; it will emit plain text */
                    colorize_line(render_ptr);
                    /* ensure buffered stdio is flushed so mixed use of write() and
                     * stdio doesn't hide output when using raw terminal writes */
                    fflush(stdout);
                }
                if (len >= (int)sizeof(tmpbuf) && render_ptr && render_ptr != E.rows[filerow] + start) free(render_ptr);
            }
        } else {
            // print blank gutter for non-existing rows, then a ~ marker
            int n = snprintf(linebuf, sizeof(linebuf), "%*s | ", ln_width, "");
            if (n > 0) write(STDOUT_FILENO, linebuf, (size_t)n);
            write(STDOUT_FILENO, "~", 1);
        }

        write(STDOUT_FILENO, "\x1b[K", 3); // clear to end of line
        if (y < content_rows - 1) write(STDOUT_FILENO, "\r\n", 2);
    }

    // If help overlay is visible, draw it on top and return
    if (E.help_visible) {
        draw_help_overlay();
        // draw help status at bottom
        char buf[64];
        int n = snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[7m", E.screenrows);
        if (n > 0) write(STDOUT_FILENO, buf, (size_t)n);
        const char *status = "\x1b[7mHelp: \x1b[0m\x1b[1;32m(u)\x1b[7m scroll up, \x1b[0m\x1b[1;34m(d)\x1b[7m scroll down, \x1b[0m\x1b[1;91m(?/ESC)\x1b[7m close\x1b[m";
        int len = (int)strlen(status);
        write(STDOUT_FILENO, status, (size_t)len);
        for (int i = len; i < E.screencols; i++) write(STDOUT_FILENO, " ", 1);
        write(STDOUT_FILENO, "\x1b[m", 3);
        return;
    }

    // If output pane is visible, render it full-page and return
    if (E.output_visible) {
        // Ensure we're showing the latest on-disk log when rendering output pane
        output_load_last_log();
        // clear and draw full page
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        // header
        char hdr[256]; int hn = snprintf(hdr, sizeof(hdr), "Output (%d lines)", E.output_n);
        char buf[512]; int pn = snprintf(buf, sizeof(buf), "\x1b[1;37m%s\x1b[0m", hdr);
        if (pn>0) write(STDOUT_FILENO, buf, (size_t)pn);
        // list available rows
        int pageSize = E.screenrows - 4; if (pageSize < 1) pageSize = 1;
        int start = E.output_scroll; if (start < 0) start = 0; if (start > E.output_n - 1) start = (E.output_n > 0) ? E.output_n - 1 : 0;
        int end = start + pageSize; if (end > E.output_n) end = E.output_n;
        for (int i = start; i < end; i++) {
            int ln = 2 + (i - start);
            char idxbuf[64]; int in = snprintf(idxbuf, sizeof(idxbuf), "\x1b[%d;1H", ln);
            if (in>0) write(STDOUT_FILENO, idxbuf, (size_t)in);
            if (i == E.output_sel) write(STDOUT_FILENO, "\x1b[7m", 4);
            // print line (truncate to screen width)
            const char *l = E.output_lines[i]; if (!l) l = "";
            int w = E.screencols; int len = (int)strlen(l); if (len > w) len = w;
            write(STDOUT_FILENO, l, (size_t)len);
            // clear rest
            char clr[32]; int cn = snprintf(clr, sizeof(clr), "\x1b[%d;%dH\x1b[K", ln, len + 1);
            if (cn>0) write(STDOUT_FILENO, clr, (size_t)cn);
            if (i == E.output_sel) write(STDOUT_FILENO, "\x1b[m", 3);
        }
        // footer with instructions
        int fn = snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[1;91m(q/ESC)\x1b[0m \x1b[1;96mclose\x1b[0m | \x1b[1;92mEnter\x1b[0m \x1b[1;96m: jump to file/line\x1b[0m | \x1b[1;94mTab\x1b[0m \x1b[1;96m: cycle errors\x1b[0m | \x1b[1;93mPgUp/PgDn\x1b[0m \x1b[1;96m: scroll\x1b[m", E.screenrows);
        if (fn>0) write(STDOUT_FILENO, buf, (size_t)fn);
        return;
    }

    // Render suggestions overlay (if any) on top of the editor
    if (SuggestionsVisible) {
        render_suggestions_overlay();
        return; // overlay drawn, skip further drawing to avoid artifacts
    }

    // If prompt overlay visible, draw it full-page
    if (E.prompt_visible) {
        write(STDOUT_FILENO, "\x1b[2J", 4); // clear screen
        write(STDOUT_FILENO, "\x1b[H", 3);  // move to top-left
        // draw message
        int msg_vis = visible_len(E.prompt_message);
        int msg_row = E.screenrows / 2 - 2;
        char buf[512]; int pn = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[37m%s\x1b[0m", msg_row, (E.screencols - msg_vis) / 2, E.prompt_message);
        if (pn > 0) write(STDOUT_FILENO, buf, (size_t)pn);
        // draw options
        int opt_vis = visible_len(E.prompt_options);
        int opt_row = msg_row + 2;
        pn = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[37m%s\x1b[0m", opt_row, (E.screencols - opt_vis) / 2, E.prompt_options);
        if (pn > 0) write(STDOUT_FILENO, buf, (size_t)pn);
        // position cursor out of way
        pn = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", E.screenrows, E.screencols);
        if (pn > 0) write(STDOUT_FILENO, buf, (size_t)pn);
        return;
    }

    // (welcome page handled earlier as full-page; skip drawing here)
    if (E.welcome_visible) return;

    // If file browser overlay visible, draw it on top and return
    // (drawn by show_file_browser when active)
    // if (E.browser_visible) { draw_file_browser(); return; }


    // position cursor (account for line-number prefix and horizontal offset)
    char buf[64];
    int base_row = conf.show_tab_bar ? 3 : 2;
    int screen_r = (E.cy - E.row_offset) + base_row; // base_row accounts for status (+tab if enabled)
    if (screen_r < base_row) screen_r = base_row;
    if (screen_r > E.screenrows) screen_r = E.screenrows;
    int c = E.cx - E.col_offset + 1 + prefix_len; // 1-indexed column plus prefix, minus offset
    if (c < prefix_len + 1) c = prefix_len + 1;
    if (c > E.screencols) c = E.screencols;
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH\x1b[?25h", screen_r, c);
    if (n > 0) write(STDOUT_FILENO, buf, (size_t)n);
}

static void editorProcessKeypress(void) {
    // If prompt visible, handle it
    if (E.prompt_visible) {
        int c = readKey();
        if (c == 'o' || c == 'O') {
            // open in current tab
            open_file_and_seek(E.pending_path, E.pending_lineno);
            E.prompt_visible = 0;
        } else if (c == 't' || c == 'T') {
            // open in new tab
            // find available tab index
            int new_idx = -1;
            for (int i = 0; i < NumTabs; i++) {
                if (!Tabs[i].name || !*Tabs[i].name) { new_idx = i; break; }
            }
            if (new_idx == -1 && NumTabs < MAX_TABS) { new_idx = NumTabs++; }
            if (new_idx != -1) {
                // save current to home if needed
                if (CurTab == -1) save_current_to_home();
                // ensure the Tabs array has space and clear any reused slot so stale cursor data does not leak in
                if (new_idx >= NumTabs) {
                    struct Tab *tmp = realloc(Tabs, sizeof(struct Tab) * (new_idx + 1));
                    if (!tmp) { snprintf(E.msg, sizeof(E.msg), "Cannot allocate tab slot"); E.msg_time = time(NULL); E.prompt_visible = 0; editorRefreshScreen(); return; }
                    Tabs = tmp;
                    // initialize any newly created slots up to new_idx
                    for (int __i = NumTabs; __i <= new_idx; __i++) Tabs[__i] = (struct Tab){0};
                    NumTabs = new_idx + 1;
                } else {
                    // reuse: free existing data and zero the slot to avoid restoring previous cursor/offsets
                    free_tab(&Tabs[new_idx]); Tabs[new_idx] = (struct Tab){0};
                }
                // switch to new tab index and open file (which will now load into a clean slot)
                CurTab = new_idx;
                open_file_and_seek(E.pending_path, E.pending_lineno);
            }
            E.prompt_visible = 0;
        } else if (c == 'c' || c == 'C' || c == 27) { // ESC or c
            E.prompt_visible = 0;
        }
        editorRefreshScreen();
        return;
    }

    // If terminal resized (SIGWINCH), refresh size and redraw immediately
    if (winch_received) { getWindowSize(); editorRefreshScreen(); winch_received = 0; return; }

    int c = readKey();
    if (c == -1) {
        // if read was interrupted by a signal, re-check window size and redraw
        if (winch_received) { getWindowSize(); editorRefreshScreen(); winch_received = 0; }
        return;
    }
    if (c == 17) { // Ctrl-Q to quit
        // clear screen before exit
        // remove swap file if present
        if (E.filename) {
            char *base = strrchr(E.filename, '/');
            const char *b = base ? base + 1 : E.filename;
            char swapbuf[1024];
            if (base) snprintf(swapbuf, sizeof(swapbuf), "%.*s/.%s.swp", (int)(base - E.filename), E.filename, b);
            else snprintf(swapbuf, sizeof(swapbuf), ".%s.swp", b);
            unlink(swapbuf);
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
    }

    // Handle welcome page input
    if (E.welcome_visible) {
        if (c == 27) { // ESC
            E.welcome_visible = 0;
            E.command_buf[0] = '\0';
            E.command_len = 0;
            free(E.filename); E.filename = NULL; editorFreeRows(); editorAppendRow(""); save_snapshot(); free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
            E.cx = 0; E.cy = 0; E.row_offset = 0; E.col_offset = 0;
            return;
        } else if (c == '\r' || c == '\n') {
            // If suggestions are visible, accept the active suggestion first
            if (SuggestionsVisible) {
                int was_filename = (ActiveFilenameStart >= 0);
                accept_active_suggestion();
                if (was_filename && E.command_len > 0) {
                    // execute command as if user pressed Enter (duplicate of below)
                    if (E.command_len == 1 && E.command_buf[0] == 'b') {
                        E.welcome_visible = 0;
                        E.browser_from_welcome = 1;
                        show_file_browser();
                    } else if (E.command_len == 1 && E.command_buf[0] == 'q') {
                        write(STDOUT_FILENO, "\x1b[2J", 4);
                        write(STDOUT_FILENO, "\x1b[H", 3);
                        disableRawMode();
                        exit(0);
                    } else if (strncmp(E.command_buf, "help", 4) == 0) {
                        E.welcome_visible = 0;
                        E.help_visible = 1;
                        E.help_scroll = 0;
                        E.help_from_welcome = 1;
                    } else if (strncmp(E.command_buf, "open ", 5) == 0) {
                        E.welcome_visible = 0;
                        push_current_to_tab_list();
                        open_file_in_current_window_no_push(&E.command_buf[5]);
                    } else if (strncmp(E.command_buf, "opent ", 6) == 0) {
                        E.welcome_visible = 0;
                        open_file_in_new_tab(&E.command_buf[6]);
                    } else if (strncmp(E.command_buf, "run ", 4) == 0) {
                        E.welcome_visible = 0;
                        run_interactive(&E.command_buf[4]);
                    } else if (E.command_len >= 2 && strncmp(E.command_buf, "R ", 2) == 0) {
                        E.welcome_visible = 0;
                        run_interactive(&E.command_buf[2]);
                    }
                    E.command_buf[0] = '\0';
                    E.command_len = 0;
                }
                return;
            }
            // execute command
            if (E.command_len > 0) {
                if (E.command_len == 1 && E.command_buf[0] == 'b') {
                    E.welcome_visible = 0;
                    E.browser_from_welcome = 1;
                    show_file_browser();
                } else if (E.command_len == 1 && E.command_buf[0] == 'q') {
                    write(STDOUT_FILENO, "\x1b[2J", 4);
                    write(STDOUT_FILENO, "\x1b[H", 3);
                    disableRawMode();
                    exit(0);
                } else if (strncmp(E.command_buf, "help", 4) == 0) {
                    E.welcome_visible = 0;
                    E.help_visible = 1;
                    E.help_scroll = 0;
                    E.help_from_welcome = 1;
                } else if (strncmp(E.command_buf, "open ", 5) == 0) {
                    E.welcome_visible = 0;
                    push_home_to_front();
                    switch_to_home();
                    open_file_in_current_window_no_push(&E.command_buf[5]);
                } else if (strncmp(E.command_buf, "opent ", 6) == 0) {
                    E.welcome_visible = 0;
                    open_file_in_new_tab(&E.command_buf[6]);
                } else if (strncmp(E.command_buf, "run ", 4) == 0) {
                    E.welcome_visible = 0;
                    run_interactive(&E.command_buf[4]);
                } else if (E.command_len >= 2 && strncmp(E.command_buf, "R ", 2) == 0) {
                    E.welcome_visible = 0;
                    run_interactive(&E.command_buf[2]);
                } else {
                    // ignore unknown commands
                }
            }
            E.command_buf[0] = '\0';
            E.command_len = 0;
            return;
        } else if (c == 127 || c == 8) { // backspace
            if (E.command_len > 0) {
                E.command_len--;
                E.command_buf[E.command_len] = '\0';
                // Update suggestions while user edits command on welcome page
                update_suggestions_from_command();
            }
            return;
        } else if (c >= 32 && c <= 126) { // printable
            if (E.command_len < (int)sizeof(E.command_buf) - 1) {
                E.command_buf[E.command_len++] = c;
                E.command_buf[E.command_len] = '\0';
                // Update suggestions while user types on the welcome page
                update_suggestions_from_command();
            }
            return;
        } else {
            // If suggestions are visible, allow navigation/acceptance with arrow keys, page keys, tab, enter, or esc
            if (SuggestionsVisible) {
                if (ActiveMatchesN <= 0) { clear_active_suggestions(); return; }
                int pageSize = (ActiveFilenameStart < 0) ? (E.screenrows - 4) : ((E.screenrows - 4 > 3) ? (E.screenrows - 4) : SuggestionPageSize);
                if (pageSize < 3) pageSize = 3;
                if (c == ARROW_DOWN) { ActiveMatchSel = (ActiveMatchSel + 1) % ActiveMatchesN; ActiveMatchesPage = ActiveMatchSel / pageSize; return; }
                if (c == ARROW_UP) { ActiveMatchSel = (ActiveMatchSel - 1 + ActiveMatchesN) % ActiveMatchesN; ActiveMatchesPage = ActiveMatchSel / pageSize; return; }
                if (c == PAGE_DOWN) { int pages = (ActiveMatchesN + pageSize - 1) / pageSize; ActiveMatchesPage = (ActiveMatchesPage + 1 < pages) ? ActiveMatchesPage + 1 : pages - 1; ActiveMatchSel = ActiveMatchesPage * pageSize; return; }
                if (c == PAGE_UP) { ActiveMatchesPage = (ActiveMatchesPage > 0) ? ActiveMatchesPage - 1 : 0; ActiveMatchSel = ActiveMatchesPage * pageSize; return; }
                if (c == '\t') { accept_active_suggestion(); return; }
                if (c == 27) { clear_active_suggestions(); return; }
            }
            return; // ignore other keys
        }
    }

    // Handle lone Escape: toggle help or mode. We already parse escape sequences
    // in readKey(), so c==27 here is a lone ESC key.
    // Special case: if output pane is visible, ESC closes it
    if (E.output_visible && c == 27) { E.output_visible = 0; return; }
    if (c == 27) {
        if (SuggestionsVisible) { clear_active_suggestions(); return; }
        if (E.help_visible) {
            E.help_visible = 0;
            if (E.help_from_welcome) {
                E.welcome_visible = 1;
                E.help_from_welcome = 0;
            }
        } else {
            // Toggle mode
            if (E.mode == MODE_VIEW) {
                E.mode = MODE_EDIT;
                // snapshot before editing
                save_snapshot();
            } else E.mode = MODE_VIEW;
            // Clear command buffer when leaving view mode
            E.command_len = 0;
            E.command_buf[0] = '\0';
        }
        return;
    }

    // If suggestions visible and in view mode, let ARROW/Enter/Tab/Esc control them
    if (SuggestionsVisible && E.mode == MODE_VIEW) {
        if (ActiveMatchesN <= 0) { clear_active_suggestions(); }
        /* compute current page size (full-page search uses more rows) */
        int pageSize = (ActiveFilenameStart < 0) ? (E.screenrows - 4) : ((E.screenrows - 4 > 3) ? (E.screenrows - 4) : SuggestionPageSize);
        if (pageSize < 3) pageSize = 3;
        if (ActiveMatchesN > 0) {
            if (c == ARROW_DOWN) { ActiveMatchSel = (ActiveMatchSel + 1) % ActiveMatchesN; ActiveMatchesPage = ActiveMatchSel / pageSize; return; }
            if (c == ARROW_UP) { ActiveMatchSel = (ActiveMatchSel - 1 + ActiveMatchesN) % ActiveMatchesN; ActiveMatchesPage = ActiveMatchSel / pageSize; return; }
            if (c == PAGE_DOWN) { int pages = (ActiveMatchesN + pageSize - 1) / pageSize; ActiveMatchesPage = (ActiveMatchesPage + 1 < pages) ? ActiveMatchesPage + 1 : pages - 1; ActiveMatchSel = ActiveMatchesPage * pageSize; return; }
            if (c == PAGE_UP) { ActiveMatchesPage = (ActiveMatchesPage > 0) ? ActiveMatchesPage - 1 : 0; ActiveMatchSel = ActiveMatchesPage * pageSize; return; }
            if (c == '\r' || c == '\n' || c == '\t') {
                int was_filename = (ActiveFilenameStart >= 0);
                accept_active_suggestion();
                if (was_filename && E.command_len > 0) {
                    // execute command in buffer
                    clear_active_suggestions();
                    E.command_buf[E.command_len] = '\0';
                    execute_command(E.command_buf);
                    E.command_len = 0;
                    E.command_buf[0] = '\0';
                }
                return;
            }
        }
        if (c == 27) { clear_active_suggestions(); return; }
    }

    // If output pane is visible, handle its keys
    if (E.output_visible) {
        int pageSize = E.screenrows - 4; if (pageSize < 1) pageSize = 1;
        if (c == 'q' || c == 27) { E.output_visible = 0; return; }
        if (c == ARROW_DOWN) { if (E.output_sel + 1 < E.output_n) { E.output_sel++; if (E.output_sel >= E.output_scroll + pageSize) E.output_scroll++; } return; }
        if (c == ARROW_UP) { if (E.output_sel > 0) { E.output_sel--; if (E.output_sel < E.output_scroll) E.output_scroll--; } return; }
        if (c == PAGE_DOWN) { E.output_scroll += pageSize; if (E.output_scroll > (E.output_n - pageSize)) E.output_scroll = (E.output_n > pageSize) ? (E.output_n - pageSize) : 0; if (E.output_sel < E.output_scroll) E.output_sel = E.output_scroll; return; }
        if (c == PAGE_UP) { E.output_scroll -= pageSize; if (E.output_scroll < 0) E.output_scroll = 0; if (E.output_sel > E.output_scroll + pageSize - 1) E.output_sel = E.output_scroll + pageSize - 1; return; }
        if (c == '\t') { // Tab: cycle to next error
            int start = E.output_sel + 1;
            if (start >= E.output_n) start = 0; // wrap around
            int found = 0;
            for (int i = 0; i < E.output_n; i++) {
                int idx = (start + i) % E.output_n;
                char path[512]; int lineno = 0, colno = 0;
                if (parse_error_location(E.output_raw_lines[idx], path, sizeof(path), &lineno, &colno)) {
                    E.output_sel = idx;
                    found = 1;
                    break;
                }
            }
            if (!found) {
                // No errors found, select first line
                if (E.output_n > 0) E.output_sel = 0;
            }
            if (E.output_sel < E.output_scroll) E.output_scroll = E.output_sel;
            else if (E.output_sel >= E.output_scroll + pageSize) E.output_scroll = E.output_sel - pageSize + 1;
            return;
        }
        if (c == '\r' || c == '\n') {
            if (E.output_sel < 0 || E.output_sel >= E.output_n) { snprintf(E.msg, sizeof(E.msg), "No line selected"); E.msg_time = time(NULL); return; }
            char *line = E.output_raw_lines[E.output_sel]; char path[512]; int lineno = 0; int colno = 0;
            if (parse_error_location(line, path, sizeof(path), &lineno, &colno)) {
                // Map temp build file back to original filename
                if (E.filename && strncmp(path, "/tmp/editor_build_temp_", 23) == 0) {
                    strncpy(path, E.filename, sizeof(path)-1);
                    path[sizeof(path)-1] = '\0';
                }
                // If the file is the currently open buffer, just seek there
                if (E.filename && E.filename[0] && strcmp(E.filename, path) == 0) {
                    editorSeekLine(lineno); E.output_visible = 0; return;
                }
                // If the file is already open in another tab, switch to that tab
                int found_tab = -1;
                if (Tabs) {
                    for (int t = 0; t < NumTabs; t++) {
                        if (!Tabs[t].name) continue;
                        if (strcmp(Tabs[t].name, path) == 0) { found_tab = t; break; }
                        char *bn = strrchr(Tabs[t].name, '/'); const char *bname = bn ? bn + 1 : Tabs[t].name;
                        if (strcmp(bname, path) == 0) { found_tab = t; break; }
                    }
                }
                if (found_tab >= 0) {
                    switch_to_tab(found_tab);
                    if (lineno > 0) editorSeekLine(lineno);
                    E.output_visible = 0; return;
                }
                // Not open: open in a new tab and seek
                open_file_in_new_tab(path);
                if (lineno > 0) editorSeekLine(lineno);
                E.output_visible = 0; return;
            } else {
                snprintf(E.msg, sizeof(E.msg), "No file:line found on this line"); E.msg_time = time(NULL); return;
            }
        }
        // any other keys ignored while output pane is open
        return;
    }

    /* Build/Run hotkeys removed: Build/Run now only execute when you type the command
       into the command buffer and press Enter (e.g., type "B" or "BR" then Enter). */

    // Navigation keys - work in both modes
    if (c == ARROW_UP) {
        /* defensive checks to prevent segfaults when buffer is empty or rows are missing */
        if (!E.rows || E.numrows <= 0) { editorScroll(); return; }
        if (E.cy > 0) {
            E.cy--;
            if (E.cy < 0) E.cy = 0;
            if (E.cy >= E.numrows) E.cy = E.numrows - 1;
            if (E.rows[E.cy]) {
                int rowlen = (int)strlen(E.rows[E.cy]);
                if (E.cx > rowlen) E.cx = rowlen;
            } else {
                E.cx = 0;
            }
        }
        E.last_edit_kind = EDIT_KIND_NONE;
        editorScroll();
        return;
    }
    if (c == ARROW_DOWN) {
        if (!E.rows || E.numrows <= 0) { editorScroll(); return; }
        if (E.cy + 1 < E.numrows) {
            E.cy++;
            if (E.cy < 0) E.cy = 0;
            if (E.cy >= E.numrows) E.cy = E.numrows - 1;
            if (E.rows[E.cy]) {
                int rowlen = (int)strlen(E.rows[E.cy]);
                if (E.cx > rowlen) E.cx = rowlen;
            } else {
                E.cx = 0;
            }
        }
        E.last_edit_kind = EDIT_KIND_NONE;
        editorScroll();
        return;
    }
    if (c == ARROW_LEFT) {
        if (E.cx > 0) {
            E.cx--;
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = strlen(E.rows[E.cy]);
            E.last_edit_kind = EDIT_KIND_NONE;
        }
        editorScroll();
        return;
    }
    // continue handling navigation keys
    if (c == ARROW_RIGHT) {
        int rowlen = (E.cy < E.numrows) ? (int)strlen(E.rows[E.cy]) : 0;
        if (E.cx < rowlen) {
            E.cx++;
        } else if (E.cy + 1 < E.numrows) {
            E.cy++;
            E.cx = 0;
            E.last_edit_kind = EDIT_KIND_NONE;
        }
        editorScroll();
        return;
    }
    if (c == HOME_KEY) { E.cx = 0; E.last_edit_kind = EDIT_KIND_NONE; editorScroll(); return; }
    if (c == END_KEY) { if (E.cy < E.numrows) E.cx = strlen(E.rows[E.cy]); E.last_edit_kind = EDIT_KIND_NONE; editorScroll(); return; }
    if (c == PAGE_UP) {
        int times = E.screenrows - 2;
        while (times-- && E.cy > 0) E.cy--;
        int rowlen = strlen(E.rows[E.cy]);
        if (E.cx > rowlen) E.cx = rowlen;
        E.last_edit_kind = EDIT_KIND_NONE;
        editorScroll();
        return;
    }
    if (c == PAGE_DOWN) {
        int times = E.screenrows - 2;
        while (times-- && E.cy + 1 < E.numrows) E.cy++;
        int rowlen2 = strlen(E.rows[E.cy]);
        if (E.cx > rowlen2) E.cx = rowlen2;
        E.last_edit_kind = EDIT_KIND_NONE;
        editorScroll();
        return;
    }

    // Mode-aware handling
    if (E.mode == MODE_EDIT) {
        if (c == 127 || c == 8) { // backspace
            editorDelChar();
            editorScroll();
        } else if (c == '\r' || c == '\n') {
            // auto-indent/newline to be implemented later; currently simple insert
            editorInsertNewline();
            editorScroll();
        } else if (c == '\t') {
            // Tab: insert either real tab or configured number of spaces
            if (conf.use_tabs) {
                editorInsertChar('\t');
            } else {
                for (int k = 0; k < conf.indent_size; k++) editorInsertChar(' ');
            }
            editorScroll();
        } else if (isprint(c)) {
            // auto-dedent for closing brace when at line start
            if (c == '}') {
                char *row = E.rows[E.cy];
                int rlen = strlen(row);
                int onlyws = 1;
                for (int i = 0; i < E.cx; i++) { if (row[i] != ' ' && row[i] != '\t') { onlyws = 0; break; } }
                if (onlyws) {
                    // Find matching open brace by scanning backwards and tracking balance, ignoring braces in strings
                    int match_row = -1;
                    int balance = 0;
                    int in_string = 0;
                    for (int r = E.cy; r >= 0 && match_row == -1; r--) {
                        int endcol = (r == E.cy) ? E.cx - 1 : (int)strlen(E.rows[r]) - 1;
                        for (int col = endcol; col >= 0; col--) {
                            char ch = E.rows[r][col];
                            if (ch == '"' || ch == '\'') {
                                in_string = !in_string;
                                continue;
                            }
                            if (in_string) continue;
                            if (ch == '}') balance++;
                            else if (ch == '{') {
                                if (balance == 0) { match_row = r; break; }
                                balance--;
                            }
                        }
                    }
                    int new_indent;
                    if (match_row != -1) {
                        new_indent = compute_leading_spaces(E.rows[match_row]);
                    } else {
                        int cur_indent = compute_leading_spaces(row);
                        new_indent = cur_indent - conf.indent_size;
                        if (new_indent < 0) new_indent = 0;
                    }
                    char *ind = make_indent_string(new_indent);
                    if (ind) {
                        int first_non = 0; while (first_non < rlen && (row[first_non] == ' ' || row[first_non] == '\t')) first_non++;
                        int newlen = strlen(ind) + (rlen - first_non);
                        char *newrow = malloc(newlen + 1);
                        if (newrow) {
                            strcpy(newrow, ind);
                            strcpy(newrow + strlen(ind), row + first_non);
                            free(E.rows[E.cy]); E.rows[E.cy] = newrow;
                            E.cx = strlen(ind);
                        }
                        free(ind);
                    }
                }
                editorInsertChar('}');
                editorScroll();
            } else if (c == '(' || c == '[' || c == '"' || c == '\'') {
                // insert pair for parens, quotes, brackets
                editorInsertChar(c);
                char pair = (c == '(') ? ')' : (c == '[') ? ']' : c;
                editorInsertChar(pair);
                // move cursor back between them
                E.cx--;
            } else if (c == '{') {
                // VSCode-like behaviour: when typing '{' after a statement, auto-insert a newline,
                // an indented inner line, and a closing '}' aligned with the statement start.
                // Insert the '{', split the line, let editorInsertNewline compute inner indent,
                // then insert a closing '}' row below aligned to the statement.
                editorInsertChar('{');
                // split into new indented inner row
                editorInsertNewline();
                // compute indent of the statement line (the line with the opening '{')
                int stmt_row = E.cy - 1;
                if (stmt_row < 0) stmt_row = 0;
                int clos_indent = compute_leading_spaces(E.rows[stmt_row]);
                char *clos_ind = make_indent_string(clos_indent);
                if (clos_ind) {
                    size_t blen = strlen(clos_ind) + 2; // indent + '}' + NUL
                    char *clos_row = malloc(blen);
                    if (clos_row) {
                        strcpy(clos_row, clos_ind);
                        strcat(clos_row, "}");
                        editorInsertRow(E.cy + 1, clos_row);
                        free(clos_row);
                    }
                    free(clos_ind);
                }
                // position cursor on the indented inner line
                editorScroll();
            } else {
                editorInsertChar(c);
            }
        }
    } else { // MODE_VIEW
        // If help overlay visible
        if (E.help_visible) {
            if (c == 27) {
                E.help_visible = 0;
            } else if (c == 'u') {
                E.help_scroll -= (E.screenrows / 2);
                if (E.help_scroll < 0) E.help_scroll = 0;
            } else if (c == 'd') {
                E.help_scroll += (E.screenrows / 2);
            }
            return;
        }


        if (c == '\r' || c == '\n') {
            // execute command in buffer
            if (E.command_len > 0) {
                // null-terminate and process
                // clear suggestions first (so overlay doesn't persist)
                clear_active_suggestions();
                E.command_buf[E.command_len] = '\0';
                execute_command(E.command_buf);
            }
            E.command_len = 0;
            E.command_buf[0] = '\0';
            return;
        }
        if (c == 127 || c == 8) { // backspace in command buffer
            if (E.command_len > 0) {
                E.command_len--;
                E.command_buf[E.command_len] = '\0';
                // update suggestions when typing filename
                update_suggestions_from_command();
            }
            return;
        }
        if (c == 27) { // ESC toggles to edit mode
            E.mode = MODE_EDIT;
            return;
        }
        if (c == '\t') {
            // Autocomplete for filename in commands like "open" and "opent".
            // If buffer starts with open/opent, find filename part and show suggestions.
            const char *cmds[] = {"open", "opent"};
            for (int ci = 0; ci < (int)(sizeof(cmds)/sizeof(cmds[0])); ci++) {
                const char *kw = cmds[ci]; size_t kwlen = strlen(kw);
                if (E.command_len >= (int)kwlen && strncmp(E.command_buf, kw, kwlen) == 0) {
                    // ensure next char is space or end
                    if (E.command_len == (int)kwlen || E.command_buf[kwlen] == ' ') {
                        // find start of filename (skip keyword and spaces)
                        int pos = (int)kwlen; while (pos < E.command_len && E.command_buf[pos] == ' ') pos++;
                        // build pattern from pos to end
                        char pat[512] = {0}; int plen = 0;
                        if (pos < E.command_len) { plen = E.command_len - pos; if (plen > (int)sizeof(pat)-1) plen = (int)sizeof(pat)-1; memcpy(pat, &E.command_buf[pos], plen); pat[plen] = '\0'; }
                        int n = 0; int max_sugg = 8; char **matches = get_fuzzy_matches(".", pat, max_sugg, &n, 0, 0, 0);
                        if (n > 0) {
                            int picked = pick_from_matches(matches, n);
                            if (picked >= 0 && picked < n) {
                                // replace buffer content from pos to end with chosen match
                                int newlen = pos + (int)strlen(matches[picked]);
                                if (newlen + 1 < (int)sizeof(E.command_buf)) {
                                    // shift/replace
                                    memcpy(&E.command_buf[pos], matches[picked], strlen(matches[picked]));
                                    E.command_len = pos + (int)strlen(matches[picked]);
                                    E.command_buf[E.command_len] = '\0';
                                }
                            }
                            free_matches(matches, n);
                        }
                        break;
                    }
                }
            }
            return;
        }
        if (isprint(c)) {
            if (E.command_len + 1 < (int)sizeof(E.command_buf)) {
                E.command_buf[E.command_len++] = (char)c;
                E.command_buf[E.command_len] = '\0';
                // update suggestions when typing filename
                update_suggestions_from_command();
            }
            return;
        }
    }
}

static void initEditor(void) {
    // load user config early
    load_config();

    E.cx = 0; E.cy = 0; E.numrows = 0; E.rows = NULL; E.row_offset = 0; E.col_offset = 0;
    E.mode = MODE_VIEW;
    E.command_len = 0;
    E.command_buf[0] = '\0';
    E.help_visible = 0;
    E.help_scroll = 0;
    E.history = NULL;
    E.history_count = 0;
    E.history_idx = -1;
    E.clipboard = NULL;
    E.batching = 0;
    E.saved_snapshot = NULL;
    E.filename = NULL;
    E.last_edit_kind = EDIT_KIND_NONE;
    E.last_edit_time_ms = 0;
    getWindowSize();
    // start with one empty line
    editorAppendRow("");
    // initial snapshot
    save_snapshot();
    // consider this initial state as saved (no unsaved changes)
    free(E.saved_snapshot);
    E.saved_snapshot = serialize_buffer();
    // show welcome page until user begins or opens a file
    E.welcome_visible = 1;
}

int main(int argc, char **argv) {
    /* Ensure terminals that honor COLORTERM/TERM get the correct hints
       so the editor emits truecolor/256-color sequences reliably even
       when the user hasn't exported them. We only set values when
       they're not already present to avoid stomping user configs. */
    if (!getenv("COLORTERM")) setenv("COLORTERM", "truecolor", 1);
    {
        const char *t = getenv("TERM");
        if (!t || (strstr(t, "256") == NULL && strstr(t, "-256color") == NULL)) {
            /* prefer xterm-256color as a reasonable default */
            setenv("TERM", "xterm-256color", 1);
        }
    }
    enableRawMode();

    /* register terminate signals to ensure terminal is restored */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_terminate_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    /* handle terminal resize */
    struct sigaction ws;
    memset(&ws, 0, sizeof(ws));
    ws.sa_handler = handle_sigwinch;
    sigaction(SIGWINCH, &ws, NULL);
    /* include update signal (from background classifier worker) */
    struct sigaction us;
    memset(&us, 0, sizeof(us));
    us.sa_handler = handle_include_update_signal;
    sigaction(SIGUSR1, &us, NULL);

    initEditor();

    // If a filename is provided on the command line, open or create it
    if (argc > 1 && argv[1]) {
        const char *fn = argv[1];
        FILE *f = fopen(fn, "r");
        if (!f) {
            // create empty buffer and set filename (file not created on disk until save)
            free(E.filename);
            E.filename = strdup(fn);
            set_language_from_filename(fn);
            editorFreeRows(); editorAppendRow("");
            save_snapshot();
            free(E.saved_snapshot); E.saved_snapshot = NULL;
            snprintf(E.msg, sizeof(E.msg), "New file %s", fn); E.msg_time = time(NULL);
        } else {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f); fseek(f, 0, SEEK_SET);
            char *buf = malloc(sz + 1);
            if (buf) {
                fread(buf, 1, sz, f);
                buf[sz] = '\0';
                load_buffer_from_string(buf);
                free(buf);
                free(E.filename);
                E.filename = strdup(fn);
                    /* detect language from filename extension and set highlighting */
                    const char *lang2 = NULL;
                    const char *ext2 = strrchr(fn, '.');
                    if (ext2) {
                        if (strcasecmp(ext2, ".c") == 0 || strcasecmp(ext2, ".h") == 0) lang2 = "c";
                        else if (strcasecmp(ext2, ".cpp") == 0 || strcasecmp(ext2, ".cc") == 0 || strcasecmp(ext2, ".cxx") == 0 || strcasecmp(ext2, ".hpp") == 0) lang2 = "cpp";
                        else if (strcasecmp(ext2, ".py") == 0) lang2 = "python";
                        else if (strcasecmp(ext2, ".js") == 0) lang2 = "javascript";
                        else if (strcasecmp(ext2, ".jsx") == 0) lang2 = "javascriptreact";
                        else if (strcasecmp(ext2, ".ts") == 0) lang2 = "typescript";
                        else if (strcasecmp(ext2, ".tsx") == 0) lang2 = "typescriptreact";
                        else if (strcasecmp(ext2, ".java") == 0) lang2 = "java";
                        else if (strcasecmp(ext2, ".go") == 0) lang2 = "go";
                        else if (strcasecmp(ext2, ".rs") == 0) lang2 = "rust";
                        else if (strcasecmp(ext2, ".rb") == 0) lang2 = "ruby";
                        else if (strcasecmp(ext2, ".php") == 0) lang2 = "php";
                        else if (strcasecmp(ext2, ".html") == 0 || strcasecmp(ext2, ".htm") == 0) lang2 = "html";
                        else if (strcasecmp(ext2, ".css") == 0) lang2 = "css";
                        else if (strcasecmp(ext2, ".json") == 0) lang2 = "json";
                        else if (strcasecmp(ext2, ".xml") == 0) lang2 = "xml";
                        else if (strcasecmp(ext2, ".yml") == 0 || strcasecmp(ext2, ".yaml") == 0) lang2 = "yaml";
                        else if (strcasecmp(ext2, ".toml") == 0) lang2 = "toml";
                        else if (strcasecmp(ext2, ".md") == 0) lang2 = "markdown";
                        else if (strcasecmp(ext2, ".sh") == 0 || strcasecmp(ext2, ".bash") == 0) lang2 = "bash";
                        else if (strcasecmp(ext2, ".zsh") == 0) lang2 = "zsh";
                        else if (strcasecmp(ext2, ".ps1") == 0) lang2 = "ps1";
                        else if (strcasecmp(ext2, ".swift") == 0) lang2 = "swift";
                        else if (strcasecmp(ext2, ".kt") == 0) lang2 = "kotlin";
                        else if (strcasecmp(ext2, ".cs") == 0) lang2 = "cs";
                        else if (strcasecmp(ext2, ".lua") == 0) lang2 = "lua";
                        else if (strcasecmp(ext2, ".r") == 0) lang2 = "r";
                        else if (strcasecmp(ext2, ".sql") == 0) lang2 = "sql";
                        else if (strcasecmp(ext2, ".pl") == 0 || strcasecmp(ext2, ".pm") == 0) lang2 = "perl";
                    }
                    if (lang2) set_language(lang2);
                save_snapshot();
                free(E.saved_snapshot); E.saved_snapshot = serialize_buffer();
                snprintf(E.msg, sizeof(E.msg), "Opened %s", fn); E.msg_time = time(NULL);
            }
            fclose(f);
        }
    }

    while (1) {
        if (winch_received) { getWindowSize(); editorScroll(); winch_received = 0; }
        if (include_update_received) { include_update_received = 0; snprintf(E.msg, sizeof(E.msg), "Include cache updated"); E.msg_time = time(NULL); }
        editorRefreshScreen();
        editorProcessKeypress();
    }
    editorFreeRows();
    return 0;
}

