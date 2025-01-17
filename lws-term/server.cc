#include "server.h"
#include "command-connect.h"

#include <sys/file.h>
#include <regex.h>
extern char **environ;

#ifndef DEFAULT_SHELL
#define DEFAULT_SHELL "/bin/bash"
#endif

static struct options opts;
struct options *main_options = &opts;
static char *default_size = NULL;
struct lws *cmdwsi = NULL;

static void make_html_file(int);
static char *make_socket_name(bool);

/** Returns a fresh copy of the (non-empty) geometry string, or NULL. */
static const char *
geometry_option(struct options *options)
{
    const char *geometry = get_setting(options->settings, "geometry");
    return geometry && geometry[0] ? geometry : default_size;
}

static FILE *_logfile = NULL;
static void lwsl_emit_stderr_with_flush(int level, const char * line) {
    char buf[50];
    lwsl_timestamp(level, buf, sizeof(buf));
    fprintf(_logfile, "%s%s", buf, line);
    fflush(_logfile);
}

static void
daemonize()
{
#if 1
    if (daemon(1, 0) != 0)
        lwsl_err("daemonizing failed\n");
#else
    if (lws_daemonize(NULL))
        lwsl_err("daemonizing failed\n");
#endif
}

void
maybe_daemonize()
{
    if (opts.do_daemonize > 0) {
        if (_logfile == NULL || _logfile == stdout || _logfile == stderr) {
            lwsl_notice("about to switch to background 'daemon' mode - no more messages.\n");
            lwsl_notice("(To see more messages use --no-daemonize option.)\n");
        } else {
            lwsl_notice("about to switch to background 'daemon' mode\n");
        }
        tty_restore(-1);
        daemonize();
        opts.do_daemonize = -1;
    }
}

int
subst_run_command(struct options *opts, const char *browser_command,
                  const char *url, int port)
{
    size_t clen = strlen(browser_command);
    const char *upos = strstr(browser_command, "%U");
    const char *gpos = strstr(browser_command, "%g");
    int skip = 2;
    char *url_tmp = NULL;
    const char *url_fixed = url;
    size_t ulen = strlen(url);
    if (upos && gpos && gpos == upos+2) {
        const char *geometry = geometry_option(opts);
        skip = 4;
        if (geometry) {
            char *tbuf = challoc(ulen+strlen(geometry)+20);
            const char *g1 = strchr(url, '#') ? "&geometry=" : "#geometry";
            sprintf(tbuf, "%s%s%s", url_fixed, g1, geometry);
	    free(url_tmp);
	    url_fixed = url_tmp = tbuf;
            ulen = strlen(url_fixed);
        }
    }
    char *cmd = challoc(clen + ulen + 40);
    const char *wpos;
    if (is_WindowsSubsystemForLinux() && strstr(browser_command, ".exe") != NULL) {
        const char *wsl_prefix = "file:///mnt/c/";
        size_t wsl_prefix_length = strlen(wsl_prefix);
        if (memcmp(url, wsl_prefix, wsl_prefix_length) == 0) {
	    char *tbuf = challoc(ulen);
            sprintf(tbuf, "file:///c:/%s", url+wsl_prefix_length);
	    free(url_tmp);
            url_fixed = url_tmp = tbuf;
	}

        const char *cp = strstr(browser_command, "cmd.exe");
        const char *sp;
        if (cp && ((sp = strchr(browser_command, ' ')) == NULL || sp > cp)) {
            char *tbuf = challoc(2 * ulen);
            const char *p0 = url_fixed;
            char *p1 = tbuf;
            const char *metas = "()%!^\"<>&| \t";
            for (;;) {
                char ch = *p0++;
                if (ch != 0 && strchr(metas, ch)) {
                    *p1++ = '^';
                }
                *p1++ = ch;
                if (ch == 0)
                    break;
            }
	    free(url_tmp);
            url_fixed = url_tmp = tbuf;
        }
    }
    if (upos) {
      size_t beforeU = upos - browser_command;
      sprintf(cmd, "%.*s%s%.*s",
              (int) beforeU, browser_command,
              url_fixed,
              (int) (clen - beforeU - skip), upos+skip);
    } else if ((wpos = strstr(browser_command, "%W")) != NULL) {
        size_t beforeW = wpos - browser_command;
        sprintf(cmd, "%.*s%d%.*s",
                (int) beforeW, browser_command,
                port,
                (int) (clen - beforeW - 2), wpos+2);
    } else
        sprintf(cmd, "%s '%s'", browser_command, url_fixed);
    free(url_tmp);
    lwsl_notice("starting frontend command: %s\n", cmd);
    return start_command(opts, cmd);
}

int start_command(struct options *opts, char *cmd) {
    arglist_t args = parse_args(cmd, true);
    const char *arg0;
    if (args != NULL) {
        arg0 = find_in_path(args[0]);
        if (arg0 == NULL) {
            printf_error(opts, "no executable front-end (browser) '%s'",
                         args[0]);
            return EXIT_FAILURE;
        }
    } else {
#if 1
        int r = system(cmd);
        if (! WIFEXITED(r) || (WEXITSTATUS(r) != 0 && ! is_WindowsSubsystemForLinux())) {
            printf_error(opts, "system could not execute %s (return code: %x)",
                         cmd, r);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
#else
        char *shell = getenv("SHELL");
        if (shell == NULL)
            shell = DEFAULT_SHELL;
        char **shell_argv = parse_args(shell, false);
        int shell_argc = count_args(shell_argv);
        args = xmalloc((shell_argc+3) * sizeof(char*));
        int i;
        for (i = 0; i < shell_argc; i++)
            args[i] = shell_argv[i];
        args[i++] = "-c";
        args[i++] = cmd[0] == '$' && cmd[1] == ' ' ? cmd + 2 : cmd;
        args[i] = NULL;
        arg0 = args[0];
#endif
    }
    pid_t pid = fork();
    if (pid == 0) {
        putenv((char*) "ELECTRON_DISABLE_SECURITY_WARNINGS=true");
#if USE_KDDockWidgets || USE_DOCK_MANAGER
        if (opts->qt_frontend) {
#ifdef QT_DOCKING_LIBDIR
            // FIXME - should append, not override
            setenv("LD_LIBRARY_PATH", QT_DOCKING_LIBDIR, 1);
#endif
            putenv((char*) "XDG_SESSION_TYPE=x11");
        }
#endif
        daemonize();
        execv(arg0, (char**) args);
        exit(-1);
    } else if (pid > 0) {// master
        free((void*) args);
    } else {
        printf_error(opts, "could not fork front-end command");
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
static int port_specified = -1;
volatile bool force_exit = false;
struct lws_context *context;
struct tty_server *server;
int http_port;
struct lws_vhost *vhost;
struct lws *focused_wsi = NULL;
struct lws_context_creation_info info;
struct cmd_client *cclient;

static const struct lws_protocols protocols[] = {
        /* http server for (mostly) static data */
        {"http-only", callback_http, sizeof(struct http_client),  0},

        /* websockets server for communicating with browser */
        {"domterm",   callback_tty,
         BROKEN_LWS_SET_WSI_USER ? sizeof(struct tty_client*) : 0,  0},

        /* callbacks for pty I/O, one pty for each session (process) */
        {"pty",       callback_pty,  sizeof(struct pty_client),  0},

        /* Unix domain socket for client to send to commands to server.
           This is the listener socket on the server. */
        {"cmd",       callback_cmd,  sizeof(struct cmd_client),  0},

#if REMOTE_SSH
        /*
          "proxy" protocol is an alternative to "domterm" in that
          it proxies between a pty_client and a file (or socket?) handle(s):
          (The pty/application runs on the "Remote" computer;
          the browser/UI runs on the "Local" computer.)
          The handles are stdout/stdin of an ssh (server) session.
          The proxy-in protocol runs on the Remote end and copies input
          (keystokes and other events) received via ssh to the pty/application.
          Output read from the pty_client is written to the file handle (stdout)
          (instead of being written to websocket client).

          If the proxy wraps a socket, there is a single "proxy" wsi.
          If the proxy is an input/output pair, then we need to
          use two struct lws instances (the "proxy" instands wraps input,
          while "proxy-out" wraps output), but they share the same
          "user-data", the same tty_client instance.
        */
        { "proxy", callback_proxy, sizeof(struct tty_client),  0},
        { "proxy-out", callback_proxy, 0,  0},
        { "ssh-stderr", callback_ssh_stderr, sizeof(struct stderr_client), 0 },
#endif

#if HAVE_INOTIFY
        /* calling back for "inotify" to watch settings.ini */
        {"inotify",    callback_inotify,  0,  0},
#endif

        {NULL,        NULL,          0,                          0}
};

#if !defined(LWS_WITHOUT_EXTENSIONS)
// websocket extensions
static const struct lws_extension extensions[] = {
        {"permessage-deflate", lws_extension_callback_pm_deflate, "permessage-deflate"},
        {"deflate-frame",      lws_extension_callback_pm_deflate, "deflate_frame"},
        {NULL, NULL, NULL}
};
#endif

#define ZIP_MOUNT "/" 

static struct lws_http_mount mount_domterm_zip = {
        NULL,                   /* linked-list pointer to next*/
        ZIP_MOUNT,              /* mountpoint in URL namespace on this vhost */
        "<change this>",      /* handler */
        "repl-client.html",   /* default filename if none given */
        NULL,
        NULL,
        NULL,
        NULL,
        0,
        0,
        0,
        0,
        0,
        0,
        LWSMPRO_FILE,   /* origin points to a callback */
        sizeof(ZIP_MOUNT)-1,
        NULL,

        { NULL, NULL } // sentinel
};

#define CHROME_OPTION 1000
#define FIREFOX_OPTION 1001
#define QTDOMTERM_OPTION 1002
#define ELECTRON_OPTION 1003
#define CHROME_APP_OPTION 1004
#define WEBVIEW_OPTION 1005
#define HEADLESS_OPTION 1006
#define VERBOSE_OPTION 1200
#define FORCE_OPTION 2001
#define DAEMONIZE_OPTION 2002
#define NO_DAEMONIZE_OPTION 2003
#define DETACHED_OPTION 2004
#define GEOMETRY_OPTION 2005
#define QT_REMOTE_DEBUGGING_OPTION 2006
#define SESSION_NAME_OPTION 2007
#define SETTINGS_FILE_OPTION 2008
#define TTY_PACKET_MODE_OPTION 2009
#define PANE_OPTIONS_START 2100
/* offsets from PANE_OPTIONS_START match 'N' in '\e[90;Nu' command */
#define PANE_OPTION (PANE_OPTIONS_START+1)
#define TAB_OPTION (PANE_OPTIONS_START+2)
#define LEFT_OPTION (PANE_OPTIONS_START+10)
#define RIGHT_OPTION (PANE_OPTIONS_START+11)
#define ABOVE_OPTION (PANE_OPTIONS_START+12)
#define BELOW_OPTION (PANE_OPTIONS_START+13)
#define PRINT_URL_OPTION (PANE_OPTIONS_START+14)
#define BROWSER_PIPE_OPTION (PANE_OPTIONS_START+15)

// command line options
static const struct option options[] = {
        {"port",         required_argument, NULL, 'p'},
        {"browser",      optional_argument, NULL, 'B'},
        {"chrome",       no_argument,       NULL, CHROME_OPTION},
        {"chrome-app",   no_argument,       NULL, CHROME_APP_OPTION},
        {"google-chrome",no_argument,       NULL, CHROME_OPTION},
        {"google-chrome",no_argument,       NULL, CHROME_OPTION},
        {"firefox",      no_argument,       NULL, FIREFOX_OPTION},
        // TODO:  "--chrome-window" --> --new-window '%U'
        // "--chrome-tab" --> --new-tab '%U'
        // "--firefox-window" --> --new-window '%U'
        // "--firefox-tab" --> --new-tab '%U'
        {"qt",           no_argument,       NULL, QTDOMTERM_OPTION},
        {"qtdomterm",    no_argument,       NULL, QTDOMTERM_OPTION},
        {"qtwebengine",  no_argument,       NULL, QTDOMTERM_OPTION},
        {"electron",     no_argument,       NULL, ELECTRON_OPTION},
        {"webview",      no_argument,       NULL, WEBVIEW_OPTION},
        {"force",        no_argument,       NULL, FORCE_OPTION},
        {"daemonize",    no_argument,       NULL, DAEMONIZE_OPTION},
        {"no-daemonize", no_argument,       NULL, NO_DAEMONIZE_OPTION},
        {"session-name", required_argument, NULL, SESSION_NAME_OPTION},
        {"sn",           required_argument, NULL, SESSION_NAME_OPTION},
        {"settings",     required_argument, NULL, SETTINGS_FILE_OPTION},
        {"tty-packet-mode",optional_argument,NULL,TTY_PACKET_MODE_OPTION},
        {"detached",     no_argument,       NULL, DETACHED_OPTION},
        {"headless",     no_argument,       NULL, HEADLESS_OPTION},
        {"geometry",     required_argument, NULL, GEOMETRY_OPTION},
        {"pane",         optional_argument, NULL, PANE_OPTION},
        {"tab",          optional_argument, NULL, TAB_OPTION},
        {"left",         optional_argument, NULL, LEFT_OPTION},
        {"right",        optional_argument, NULL, RIGHT_OPTION},
        {"above",        optional_argument, NULL, ABOVE_OPTION},
        {"below",        optional_argument, NULL, BELOW_OPTION},
        {"print-url",    no_argument,       NULL, PRINT_URL_OPTION},
#if REMOTE_SSH
        {"browser-pipe", no_argument,       NULL, BROWSER_PIPE_OPTION},
#endif
        {"socket-name",  required_argument, NULL, 'L'},
        {"interface",    required_argument, NULL, 'i'},
        {"credential",   required_argument, NULL, 'c'},
        {"uid",          required_argument, NULL, 'u'},
        {"gid",          required_argument, NULL, 'g'},
        {"signal",       required_argument, NULL, 's'},
        {"reconnect",    required_argument, NULL, 'r'},
#if HAVE_OPENSSL
        {"ssl",          no_argument,       NULL, 'S'},
        {"ssl-cert",     required_argument, NULL, 'C'},
        {"ssl-key",      required_argument, NULL, 'K'},
        {"ssl-ca",       required_argument, NULL, 'A'},
#endif
        {"readonly",     no_argument,       NULL, 'R'},
        {"check-origin", no_argument,       NULL, 'O'},
        {"once",         no_argument,       NULL, 'o'},
        {"debug",        required_argument, NULL, 'd'},
        {"remote-debugging-port", required_argument, NULL, QT_REMOTE_DEBUGGING_OPTION},

        {"version",      no_argument,       NULL, 'v'},
        {"verbose",      no_argument,       NULL, VERBOSE_OPTION },
        {"help",         no_argument,       NULL, 'h'},
        {NULL, 0, 0,                              0}
};
static const char *opt_string = "+p:B::i:c:u:g:s:r:aSC:K:A:Rt:Ood:L:vh";

static struct tty_server *
tty_server_new() {
    struct tty_server *ts = (struct tty_server *) xmalloc(sizeof(struct tty_server));

    memset(ts, 0, sizeof(struct tty_server));
    ts->session_count = 0;

    return ts;
}

void
tty_server_free(struct tty_server *ts) {
    if (ts == NULL)
        return;
    if (ts->options.credential != NULL)
        free(ts->options.credential);
    if (ts->options.sig_name)
        free(ts->options.sig_name);
    if (ts->socket_path != NULL) {
        struct stat st;
        if (!stat(ts->socket_path, &st)) {
            unlink(ts->socket_path);
        }
        free(ts->socket_path);
    }
    free(ts);
}

void
sig_handler(int sig) {
    if (force_exit)
        exit(EXIT_FAILURE);

    char sig_name[20];
    get_sig_name(sig, sig_name);
    lwsl_notice("received signal: %s (%d), exiting...\n", sig_name, sig);
    force_exit = true;
    tty_restore(-1);
    if (context)
        lws_cancel_service(context);
    lwsl_notice("send ^C to force exit.\n");
}

char *
get_bin_relative_path(const char* app_path)
{
    char* path = get_executable_path();
    int dirname_length = get_executable_directory_length();

    if (dirname_length > 4 && memcmp(path+dirname_length-4, "/bin", 4)==0)
      dirname_length -= 4;

    int app_path_length = strlen(app_path);
    char *buf = (char*)xmalloc(dirname_length + app_path_length + 1);
    sprintf(buf, "%.*s%s", dirname_length, path, app_path);
    return buf;
}

/* Returns freshly allocated string or NULL. */
char *
find_in_path(const char *name)
{
    if (index(name, '/') && access(name, X_OK) == 0)
        return strdup(name);
    // FIXME: if (strchr(name, '/') prepend working directory
    char *path = getenv("PATH");
    int plen = strlen(path);
    char *end = path + plen;
    char *buf = challoc(plen + strlen(name) + 2);
    for (;;) {
        char* colon = strchr(path, ':');
        if (colon == NULL)
            colon = end;
        if (path != colon) {
             sprintf(buf, "%.*s/%s", (int) (colon-path), path, name);
            if (access(buf, X_OK) == 0)
                return buf;
        }
        if (colon == end)
            return NULL;
        path = colon + 1;
    }
}

char *
fix_for_windows(char * fname)
{
    if (is_WindowsSubsystemForLinux()) {
        int fname_length = strlen(fname);
        static char mnt_prefix[] = "/mnt/";
        static int mnt_prefix_length = sizeof(mnt_prefix)-1;
        if (memcmp(fname, mnt_prefix, mnt_prefix_length) == 0
            && fname_length > mnt_prefix_length) {
            char *buf = challoc(fname_length);
            sprintf(buf, "%c:/%s", fname[mnt_prefix_length], fname+mnt_prefix_length+1);
            return buf;
        }
    }
    return fname;
}

/** Return freshly allocated command string or NULL */
const char *
chrome_command(bool app_mode, struct options *options)
{
    bool free_needed = false;
    const char *chrome_cmd = get_setting(options->settings, "command.chrome");
    if (chrome_cmd == NULL && (chrome_cmd = getenv("CHROME_BIN")) != NULL
        && access(chrome_cmd, X_OK) == 0) {
        const char *c = maybe_quote_arg(chrome_cmd);
        free_needed = c != chrome_cmd;
        chrome_cmd = c;
    }
    if (chrome_cmd == NULL) {
        char *pbin = find_in_path("chrome");
        if (pbin == NULL)
            pbin = find_in_path("google-chrome");
        if (pbin != NULL) {
            chrome_cmd = maybe_quote_arg(pbin);
            free_needed = chrome_cmd != pbin;
        }
    }
    if (chrome_cmd == NULL && is_WindowsSubsystemForLinux()) {
#define CHROME_EXE "/mnt/c/Program Files (x86)/Google/Chrome/Application/chrome.exe"
	if (access(CHROME_EXE, X_OK) == 0) {
            chrome_cmd = "'" CHROME_EXE "'";
        }
    }
#if __APPLE__
    // FIXME - better to open -a "Google Chrome" OR open -b com.google.Chrome
#define CHROME_MAC "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if (chrome_cmd == NULL && access(CHROME_MAC, X_OK) == 0) {
        chrome_cmd = "/usr/bin/open -a '" CHROME_MAC "'";
    }
#endif
    if (chrome_cmd == NULL)
        return NULL;
    struct sbuf sb;
    sbuf_init(&sb);
    sbuf_append(&sb, chrome_cmd, -1);
    if (free_needed)
        free((void*) chrome_cmd);
    if (options->headless)
        sbuf_append(&sb, " --headless --remote-debugging-port=0 '%U'", -1);
    else if (app_mode)
        sbuf_append(&sb, " --app='%U%g'", -1);
    char *buf = sbuf_strdup(&sb);
    sbuf_free(&sb);
    return buf;
}

const char *
firefox_browser_command(struct options *options)
{
    const char *firefox_cmd = get_setting(options->settings, "command.firefox");
    if (firefox_cmd != NULL)
        return firefox_cmd;
    if (is_WindowsSubsystemForLinux())
        return "'/mnt/c/Program Files (x86)/Mozilla Firefox/firefox.exe'";
    char *firefoxCommand = find_in_path("firefox");
    if (firefoxCommand != NULL)
        return firefoxCommand;
#define firefoxMac "/Applications/Firefox.app"
    if (access(firefoxMac, X_OK) == 0)
      return "/usr/bin/open -a " firefoxMac;
    return NULL;
}

const char *
firefox_command(struct options *options)
{
    const char *firefox = firefox_browser_command(options);
    return firefox ? firefox : "firefox";
}

/** Return freshly allocated command string or NULL */
static char *
qtwebengine_command(struct options *options)
{
    char *cmd = get_bin_relative_path("/bin/qtdomterm");
    if (cmd == NULL || access(cmd, X_OK) != 0) {
        if (cmd == NULL)
            free(cmd);
        return NULL;
    }
    struct sbuf sb;
    sbuf_init(&sb);
    const char *geometry = geometry_option(options);
    sbuf_append(&sb, cmd, -1);
    free(cmd);
    if (geometry)
        sbuf_printf(&sb, " --geometry %s", geometry);
    if (options->qt_remote_debugging)
        sbuf_printf(&sb, " --remote-debugging-port=%s", options->qt_remote_debugging);
    if (options->headless)
        sbuf_append(&sb, " --headless", -1);
    sbuf_append(&sb, " --connect '%U'", -1);
    char *buf = sbuf_strdup(&sb);
    sbuf_free(&sb);
    return buf;
}

/** Return freshly allocated command string or NULL */
static char *
webview_command(struct options *options)
{
    char *cmd = get_bin_relative_path("/bin/dt-webview");
    if (cmd == NULL || access(cmd, X_OK) != 0) {
        if (cmd == NULL)
            free(cmd);
        return NULL;
    }
    int bsize = strlen(cmd)+100;
    const char *geometry = geometry_option(options);
    if (geometry)
        bsize += strlen(geometry);
    char *buf = challoc(bsize);
    strcpy(buf, cmd);
    free(cmd);
    if (geometry) {
        strcat(buf, " --geometry ");
        strcat(buf, geometry);
    }
    strcat(buf, " '%U'");
    return buf;
}

/** Return freshly allocated command string or NULL */
static char *
electron_command(struct options *options)
{
    char *epath_free_needed = NULL;
    const char *epath = get_setting(options->settings, "command.electron");
    if (epath == NULL) {
        char *ppath = find_in_path("electron");
        if (ppath == NULL && is_WindowsSubsystemForLinux())
            ppath = find_in_path("electron.exe");
        if (ppath == NULL)
            return NULL;
        epath_free_needed = realpath(ppath, NULL);
        epath = epath_free_needed;
        free(ppath);

    }
    char *app = get_bin_relative_path(DOMTERM_DIR_RELATIVE "/electron");
    char *app_fixed = fix_for_windows(app);
    struct sbuf sb;
    sbuf_init(&sb);
#ifdef __APPLE__
    sbuf_printf(&sb, "/usr/bin/open -a %s --args", epath);
#else
    sbuf_printf(&sb, "%s", epath);
#endif
    sbuf_printf(&sb, " %s", app_fixed);
    const char *geometry = geometry_option(options);
    if (geometry)
        sbuf_printf(&sb, " --geometry %s", geometry);
    if (options->headless)
        sbuf_printf(&sb, " --headless");
    sbuf_append(&sb, " --url '%U'", -1);
    if (app_fixed != app)
        free(app_fixed);
    if (epath_free_needed)
        free(epath_free_needed);
    char *buf = sbuf_strdup(&sb);
    sbuf_free(&sb);
    return buf;
}

int
default_browser_run(const char *url, int port, struct options *options)
{
    const char *pattern;
    char *mpattern = NULL; // malloc'd pattern
#ifdef DEFAULT_BROWSER_COMMAND
    pattern = DEFAULT_BROWSER_COMMAND;
#elif __APPLE__
    pattern = "open";
#else
    // Prefer gnome-open or kde-open over xdg-open because xdg-open
    // under Gnome defaults to using 'gio open', which does drops the "hash"
    // part of a "file:" URL, and may also use a non-preferred browser.
    if (is_WindowsSubsystemForLinux()) {
        pattern = "/mnt/c/Windows/System32/cmd.exe /c start '%U'";
    } else {
        mpattern = find_in_path("xdg-open");
        if (mpattern == NULL)
            mpattern = find_in_path("kde-open");
        if (mpattern == NULL) {
            pattern = "firefox";
        } else
            pattern = mpattern;
    }
#endif
    int r = subst_run_command(options, pattern, url, 0);
    free(mpattern);
    return r;
}

void
default_link_command(const char *url)
{
#if !defined(DEFAULT_BROWSER_COMMAND) && (defined(_WIN32)||defined(__CYGWIN__))
    ShellExecute(0, 0, url, 0, 0 , SW_SHOW) > 32 ? 0 : 1;
#else
    default_browser_run(url, 0, main_options);
#endif
}

/** Request browser client to open new browser window. */
void
browser_run_browser(struct options *options, const char *url,
                    struct tty_client *tclient)
{
    struct json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "url",  json_object_new_string(url));
    const char *geometry = geometry_option(options);
    if (geometry)
        json_object_object_add(jobj, "geometry", json_object_new_string(geometry));
    if (options->headless)
        json_object_object_add(jobj, "headless", json_object_new_boolean(1));
    const char *json_data = json_object_to_json_string_ext(jobj, JSON_C_TO_STRING_PLAIN);
    printf_to_browser(tclient,
                      URGENT_START_STRING "\033]108;%s\007" URGENT_END_STRING,
                      json_data);
    json_object_put(jobj);
}

int
do_run_browser(struct options *options, const char *url, int port)
{
    const char *browser_specifier;
    if (options != NULL && options->browser_command != NULL) {
        browser_specifier = options->browser_command;
        opts.browser_command = browser_specifier;
    } else {
        browser_specifier = opts.browser_command;
    }
    bool do_electron = false, do_Qt = false;
    if (browser_specifier == NULL && port_specified < 0) {
        const char *default_frontend = get_setting(options->settings, "frontend.default");
        if (default_frontend == NULL)
            default_frontend = "electron;qt;chrome-app;firefox;browser";
        const char *p = default_frontend;
        for (;;) {
            const char *argv0_end = NULL;
            const char *start = NULL;
            const char *end = NULL;
            const char *semi = extract_command_from_list(p, &start, &end, &argv0_end);
            int cmd_length = end-start;
            if (cmd_length > 0) {
                char *cmd = challoc(cmd_length + 1);
                memcpy(cmd, start, cmd_length);
                cmd[cmd_length] = '\0';
                int argv0_length = argv0_end-start;
                bool app_mode;
                if (strcmp(cmd, "electron") == 0) {
                    browser_specifier = electron_command(options);
                     if (browser_specifier != NULL) {
                         free (cmd);
                         do_electron = true;
                         break;
                     }
                } else if (strcmp(cmd, "webview") == 0) {
                    browser_specifier = webview_command(options);
                    if (browser_specifier != NULL) {
                        break;
                    }
                } else if (strcmp(cmd, "qt") == 0
                           || strcmp(cmd, "qtdomterm") == 0
                           || strcmp(cmd, "qtwebengine") == 0) {
                    browser_specifier = qtwebengine_command(options);
                    if (browser_specifier != NULL) {
                        free (cmd);
                        do_Qt = true;
                        options->qt_frontend = true;
                        break;
                    }
                } else if (strcmp(cmd, "firefox") == 0) {
                    browser_specifier = firefox_browser_command(options);
                    if (browser_specifier != NULL)
                        break;
                } else if ((app_mode = ! strcmp(cmd, "chrome-app"))
                           || ! strcmp(cmd, "chrome")
                           || ! strcmp(cmd, "google-chrome")) {
                    browser_specifier = chrome_command(app_mode, options);
                    if (browser_specifier != NULL)
                        break;
                } else {
                    char save_argv0_end = cmd[argv0_length];
                    cmd[argv0_length] = '\0';
                    browser_specifier = find_in_path(cmd);
                    cmd[argv0_length] = save_argv0_end;
                    if (browser_specifier != NULL) {
                        browser_specifier = cmd;
                        break;
                    }
                }
                free(cmd);
            }
            if (*semi == 0) {
                fprintf(stderr, "no front-end command found\n");
                exit(-1);
            }
            p = semi+1;
        }
    }
    bool app_mode;
    if (strcmp(browser_specifier, "--qtwebengine") == 0) {
        browser_specifier = qtwebengine_command(options);
        if (browser_specifier == NULL) {
            printf_error(options, "'qtdomterm' missing");
            return EXIT_FAILURE;
        }
        options->qt_frontend = true;
        do_Qt = true;
    }
    else if (strcmp(browser_specifier, "--electron") == 0) {
        browser_specifier = electron_command(options);
        do_electron = true;
        if (browser_specifier == NULL) {
            printf_error(options, "'electron' not found in PATH");
            return EXIT_FAILURE;
        }
    } else if ((app_mode = ! strcmp(browser_specifier, "--chrome-app"))
             || ! strcmp(browser_specifier, "--chrome")
             || ! strcmp(browser_specifier, "--google-chrome")) {
        browser_specifier = chrome_command(app_mode, options);
            if (browser_specifier == NULL) {
                printf_error(options,
                             "neither chrome or google-chrome command found");
                return EXIT_FAILURE;
            }
    }
    else if (options->headless) {
        const char *hcmd = get_setting(options->settings, "command.headless");
        if (hcmd) {
            browser_specifier = hcmd;
        } else if ((hcmd = chrome_command(true, options)) != NULL) {
            // default to chrome for headless
            browser_specifier = hcmd;
        } else {
             printf_error(options, "unspecified browser for --headless");
             return EXIT_FAILURE;
        }
    }
    if (strcmp(browser_specifier, "--firefox") == 0)
        browser_specifier = firefox_command(options);
    else if (strcmp(browser_specifier, "--webview") == 0) {
        browser_specifier = webview_command(options);
        if (browser_specifier == NULL) {
                printf_error(options,
                             "cannot find dt-webview command");
                return EXIT_FAILURE;
            }
    }

    const char *do_pattern = do_electron ? "\"electron\":\""
        : do_Qt ? "\"qtwebengine\":\""
        : NULL;
    // If there is an existing Electron or Qt instance, we want to re-use it.
    // Otherwise, on Qt we get a multi-second delay on startup.
    // This is no longer needed on Electron, but is a slight optimization.
    // Other browsers seem to "combine" user commands better.
    if (do_pattern) {
        struct tty_client *t;
        FORALL_WSCLIENT(t) {
            if (t->version_info && strstr(t->version_info, do_pattern)) {
                browser_run_browser(options, url, t);
                lws_callback_on_writable(t->wsi);
                return EXIT_SUCCESS;
            }
        }
    }

    if (browser_specifier[0] == '\0')
        return default_browser_run(url, port, options);
    int r = subst_run_command(options, browser_specifier, url, port);
    options->qt_frontend = false;
    // FIXME: free(browser_specifier)
    return r;
}

char *
get_domterm_jar_path()
{
    return get_bin_relative_path(DOMTERM_DIR_RELATIVE "/domterm.jar");
}

options::options()
{
    browser_command = NULL;
    headless = false;
    http_server = false;
    something_done = false;
    verbosity = 0;
    paneOp = -1;
    force_option = 0;
    socket_name = NULL;
    do_daemonize = 1;
    debug_level = 0;
    iface = NULL;
    tty_packet_mode = "no";
#if HAVE_OPENSSL
    ssl = false;
    cert_path = NULL;
    key_path = NULL;
    ca_path = NULL;
    credential = NULL;
#endif
    once = false;
    reconnect = 10;
    sig_code = SIGHUP;
    sig_name = NULL; // FIXME
    qt_remote_debugging = NULL;
    fd_in = STDIN_FILENO;
    fd_out = STDOUT_FILENO;
    fd_err = STDERR_FILENO;
    fd_cmd_socket = -1;
    session_name = NULL;
    settings_file = NULL;
    shell_argv = NULL;
    env = NULL;
    cwd = NULL;
    remote_output_interval = 0;
    remote_input_timeout = 0;
    remote_output_timeout = 0;
}

options::~options()
{
    // FIXME implement to fix memory leaks
    free((void*) env);
    free((void*) cwd);
    if (cmd_settings)
        json_object_put(cmd_settings);
}

struct options *link_options(struct options *opts)
{
    if (opts) {
        opts->reference_count++;
    } else {
        opts = new struct options;
        opts->reference_count = 1;
    }
    return opts;
}

void
options::release(struct options *opts)
{
    if (opts != main_options && --opts->reference_count == 0) {
        delete opts;
    }
}

static arglist_t default_argv = NULL;

arglist_t default_command(struct options *opts)
{
    if (opts != NULL && opts->shell_argv != NULL)
        return opts->shell_argv;
    else
        return default_argv;
}

void prescan_options(int argc, arglist_t argv, struct options *opts)
{
    // parse command line options
    optind = 1;
    opterr = 0;
    for (;;) {
        int c = getopt_long(argc, (char * const*) argv, opt_string, options, NULL);
        if (c == -1) {
            if (optind < argc && check_option_arg(argv[optind], opts)) {
                optind++;
                continue;
            }
            //fprintf(stderr, "after args optind:%d argc:%d next:%s\n", optind, argc, argv[optind]);
            break;
        }
        if (c == '?')
            break;
        switch (c) {
        case SETTINGS_FILE_OPTION:
            opts->settings_file = optarg;
            break;
        case VERBOSE_OPTION:
            opts->verbosity++;
            break;
        case 'd':
            opts->debug_level = atoi(optarg);
            break;
        }
    }
    opterr = 1;
}

void
print_version (FILE *out)
{
    if (git_describe[0])
        fprintf(out, "DomTerm version %s (git describe: %s)\n",
               LDOMTERM_VERSION, git_describe);
    else
        fprintf(out, "DomTerm version %s\n",
               LDOMTERM_VERSION);
    fprintf(out, "Copyright %s Per Bothner and others\n", LDOMTERM_YEAR);
#ifdef LWS_LIBRARY_VERSION
    fprintf(out, "Using Libwebsockets " LWS_LIBRARY_VERSION "\n");
#endif
}

int process_options(int argc, arglist_t argv, struct options *opts)
{
    // parse command line options
    optind = 1;
    int c;
    for (;;) {
        int c = getopt_long(argc, (char * const*)argv, opt_string, options, NULL);
        if (c == -1) {
            const char *eq = optind >= argc ? NULL : strchr(argv[optind], '=');
            if (eq) {
                optind++;
                continue;
            }
            break;
        }
        switch (c) {
            case 'h':
                print_help(stderr);
                opts->something_done = true;
                break;
            case 'v':
                print_version(stdout);
                opts->something_done = true;
                break;
            case 'R':
                opts->readonly = true;
                break;
            case 'O':
                opts->check_origin = true;
                break;
            case 'o':
                opts->once = true;
                break;
            case 'p':
                info.port = atoi(optarg);
                port_specified = info.port;
                if (info.port < 0) {
                    fprintf(stderr, "domterm: invalid port: %s\n", optarg);
                    return -1;
                }
                break;
            case 'B':
                opts->browser_command = optarg == NULL ? "" : optarg;
                break;
            case FORCE_OPTION:
                opts->force_option = 1;
                break;
            case NO_DAEMONIZE_OPTION:
            case DAEMONIZE_OPTION:
                opts->do_daemonize = (c == DAEMONIZE_OPTION);
                break;
            case SESSION_NAME_OPTION:
                opts->session_name = optarg;
                break;
            case TTY_PACKET_MODE_OPTION:
                if (optarg != NULL) {
#if !defined(TIOCPKT)
                    if (strcmp(optarg, "yes") == 0)
                        fprintf(stderr, "warning - tty package mode not available\n");
#endif
#if ! defined(EXTPROC)
                    if (strcmp(optarg, "extproc") == 0)
                        fprintf(stderr, "warning - tty package mode EXTPROC not available\n");
#endif
                }
                opts->tty_packet_mode = optarg == NULL ? "yes" : optarg;
                break;
            case VERBOSE_OPTION:
            case SETTINGS_FILE_OPTION:
            case 'd':
                break; // handled in prescan_options
            case TAB_OPTION:
            case LEFT_OPTION:
            case RIGHT_OPTION:
            case BELOW_OPTION:
            case ABOVE_OPTION:
                opts->paneOp = c - PANE_OPTIONS_START;
                opts->browser_command = argv[optind-1];
                break;
            case PANE_OPTION:
            case PRINT_URL_OPTION:
#if REMOTE_SSH
            case BROWSER_PIPE_OPTION:
#endif
                opts->paneOp = c - PANE_OPTIONS_START;
                /* ... fall through ... */
            case DETACHED_OPTION:
            case ELECTRON_OPTION:
            case FIREFOX_OPTION:
                opts->browser_command = argv[optind-1];
                break;
            case HEADLESS_OPTION:
                opts->headless = true;
                break;
            case GEOMETRY_OPTION: {
                regex_t rx;
#define SZ_REGEX "[0-9]+x[0-9]+"
#define POS_REGEX "[-+][0-9]+[-+][0-9]+"
#define GEOM_REGEX "^" SZ_REGEX "$|^" POS_REGEX "$|^" SZ_REGEX POS_REGEX "$"
                regcomp(&rx, GEOM_REGEX, REG_EXTENDED|REG_NOSUB);
                if (regexec(&rx, optarg, 0, NULL, 0)) {
                    fprintf(stderr, "bad geometry specifier '%s'\n", optarg);
                    fprintf(stderr, "must have the form: [{WIDTH}x{HEIGHT}][{+-}{XOFF}{+-}{YOFF}]\n");
                    exit(-1);
                }
                regfree(&rx);
                //opts->geometry = optarg;
                set_setting(&opts->cmd_settings, "geometry", optarg);

                free(default_size);
                char *p = optarg;
                char ch;
                while ((ch = *p) != 0 && ch != '-' && ch != '+')
                    p++;
                ptrdiff_t len = p - optarg;
                if (len == 0)
                    default_size = NULL;
                else {
                    char *r = challoc(len + 1);
                    memcpy(r, optarg, len);
                    r[len] = 0;
                    default_size = r;
                }
            }
                break;
            case CHROME_OPTION:
                opts->browser_command = "--chrome";
                break;
            case CHROME_APP_OPTION:
                opts->browser_command = "--chrome-app";
                break;
            case QTDOMTERM_OPTION:
                opts->browser_command = "--qtwebengine";
                break;
            case WEBVIEW_OPTION:
                opts->browser_command = "--webview";
                break;
            case QT_REMOTE_DEBUGGING_OPTION:
                opts->qt_remote_debugging = strdup(optarg);
                break;
            case 'L':
                opts->socket_name = strdup(optarg);
                break;
            case 'i':
                if (opts->iface != NULL)
                    free(opts->iface);
                break;
            case 'c':
                if (strchr(optarg, ':') == NULL) {
                    fprintf(stderr, "domterm: invalid credential, format: username:password\n");
                    return -1;
                }
                opts->credential = base64_encode((const unsigned char *) optarg, strlen(optarg));
                break;
            case 'u':
                info.uid = atoi(optarg);
                break;
            case 'g':
                info.gid = atoi(optarg);
                break;
            case 's': {
                int sig = get_sig(optarg);
                if (sig > 0) {
                    opts->sig_code = get_sig(optarg);
                    opts->sig_name = uppercase(strdup(optarg));
                } else {
                    fprintf(stderr, "domterm: invalid signal: %s\n", optarg);
                    return -1;
                }
            }
                break;
            case 'r':
                opts->reconnect = atoi(optarg);
                if (opts->reconnect <= 0) {
                    fprintf(stderr, "domterm: invalid reconnect: %s\n", optarg);
                    return -1;
                }
                break;
#if HAVE_OPENSSL
            case 'S':
                opts->ssl = true;
                break;
            case 'C':
                if (opts->cert_path != NULL)
                    free(opts->cert_path);
                opts->cert_path = strdup(optarg);
                break;
            case 'K':
                if (opts->key_path != NULL)
                    free(opts->key_path);
                opts->key_path = strdup(optarg);
                break;
            case 'A':
                if (opts->ca_path != NULL)
                    free(opts->ca_path);
                opts->ca_path = strdup(optarg);
                break;
#endif
            case '?':
                return -1;
            default:
                print_help(stderr);
                return -1;
        }
    }
    return 0;
}

int
main(int argc, char **argv)
{
    memset(&info, 0, sizeof(info));
#if LWS_LIBRARY_VERSION_NUMBER <= 2004002
    // See "problems with big dynamic content" thread on libwebsockets list
    info.keepalive_timeout = 0x7fffffff;
#endif
    info.port = 0;
    info.iface = NULL;
    info.protocols = protocols;
#if HAVE_OPENSSL
    info.ssl_cert_filepath = NULL;
    info.ssl_private_key_filepath = NULL;
#endif
    info.gid = -1;
    info.uid = -1;
    info.max_http_header_pool = 16;
    info.options = LWS_SERVER_OPTION_VALIDATE_UTF8|LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
#if !defined(LWS_WITHOUT_EXTENSIONS)
    info.extensions = extensions;
#endif
    info.timeout_secs = 5;
#ifdef RESOURCE_DIR
    mount_domterm_zip.origin = get_resource_path();
#endif
    info.mounts = &mount_domterm_zip;

    const char *shell = getenv("SHELL");
    if (shell == NULL)
        shell = DEFAULT_SHELL;
    default_argv = parse_args(shell, false);

    prescan_options(argc, (arglist_t) argv, &opts);

    read_settings_file(&opts, false);
    set_settings(&opts);

    int debug_level = opts.debug_level;
    const char *logfilefmt = get_setting(opts.settings, "log.file");
    if (debug_level == 0 && opts.verbosity > 0) {
         debug_level = LLL_ERR|LLL_WARN|LLL_NOTICE
             |(opts.verbosity > 1 ? LLL_INFO : 0);
         if (logfilefmt == NULL)
             logfilefmt = "notimestamp";
    }
    if (debug_level == 0) {
        const char *to_server = get_setting(opts.settings, "log.js-to-server");
        if (to_server && (strcmp(to_server, "true") == 0
                          || strcmp(to_server, "yes") == 0
                          || strcmp(to_server, "both") == 0))
            debug_level = LLL_ERR|LLL_NOTICE;
    }

    if (logfilefmt == NULL)
        logfilefmt = "/tmp/domterm-%P.log";
    if (debug_level == 0)
        lws_set_log_level(debug_level, NULL);
    else if (strcmp(logfilefmt, "stderr") == 0)
        lws_set_log_level(debug_level, lwsl_emit_stderr);
    else if (strcmp(logfilefmt, "stdout") == 0) {
        _logfile = stdout;
        lws_set_log_level(debug_level, lwsl_emit_stderr_with_flush);
    } else if (strcmp(logfilefmt, "stderr-notimestamp") == 0
               || strcmp(logfilefmt, "notimestamp") == 0)
        lws_set_log_level(debug_level, lwsl_emit_stderr_notimestamp);
    else {
        struct sbuf sb;
        sbuf_init(&sb);
        const char *p = logfilefmt;
        for (; *p; p++) {
            const char *pc = strchr(p, '%');
            if (pc == NULL) {
                sbuf_append(&sb, p, strlen(p));
                break;
            }
            sbuf_append(&sb, p, pc-p);
            p = pc+1;
            if (pc[1] == 'P') {
                sbuf_printf(&sb, "%d", getpid());
            } else if (pc[1] == '%') {
                sbuf_append(&sb, "%", 1);
            }
        }
        sbuf_append(&sb, "", 1);
        _logfile = fopen((const char *) sb.buffer, "a");
        sbuf_free(&sb);
        lws_set_log_level(debug_level, lwsl_emit_stderr_with_flush);
    }

    lwsl_notice("domterm terminal server %s (git describe: %s)\n",
                LDOMTERM_VERSION, git_describe);
    lwsl_notice("Copyright %s Per Bothner and others\n", LDOMTERM_YEAR);
#ifdef LWS_LIBRARY_VERSION
    lwsl_notice("Using Libwebsockets " LWS_LIBRARY_VERSION "\n");
#endif

    read_settings_emit_notice();

    if (process_options(argc, (arglist_t) argv, &opts) != 0)
        return -1;
    if (opts.something_done && argv[optind] == NULL)
        exit(0);

    signal(SIGINT, sig_handler);  // ^C
    signal(SIGTERM, sig_handler); // kill

    const char *cmd = argv[optind];
    struct command *command = cmd == NULL ? NULL : find_command(cmd);
    if (command == NULL && cmd != NULL && index(cmd, '/') == NULL
#if REMOTE_SSH
        && strchr(cmd, '@') == NULL
#endif
        ) {
        fprintf(stderr, "domterm: unknown command '%s'\n", cmd);
        exit(EXIT_FAILURE);
    }
    if (command && (command->options & COMMAND_IN_SERVER) != 0
        && (command->options & COMMAND_CHECK_DOMTERM) != 0) {
        check_domterm(&opts);
    }
    int socket = -1;
    if ((command == NULL ||
         (command->options &
          (COMMAND_IN_CLIENT_IF_NO_SERVER|COMMAND_IN_SERVER)) != 0))
      socket = client_connect(make_socket_name(false));
    if (command != NULL
        && ((command->options & COMMAND_IN_CLIENT) != 0
            || ((command->options & COMMAND_IN_CLIENT_IF_NO_SERVER) != 0
                && socket < 0))) {
        lwsl_notice("handling command '%s' locally\n", command->name);
        exit((*command->action)(argc-optind, (arglist_t)argv+optind, NULL, &opts));
    }
    if (socket >= 0) {
        exit(client_send_command(socket, argc, argv, environ));
    }

    server = tty_server_new();
    server->options = opts;

    if (port_specified < 0)
        server->client_can_close = true;

#if LWS_LIBRARY_VERSION_MAJOR >= 2
    char server_hdr[128] = "";
    sprintf(server_hdr, "domterm/%s (libwebsockets/%s)", LDOMTERM_VERSION, LWS_LIBRARY_VERSION);
    info.server_string = server_hdr;
#endif

#if 0
    if (strlen(iface) > 0) {
        info.iface = iface;
        if (endswith(info.iface, ".sock") || endswith(info.iface, ".socket")) {
#ifdef LWS_USE_UNIX_SOCK
            info.options |= LWS_SERVER_OPTION_UNIX_SOCK;
            server->socket_path = strdup(info.iface);
#else
            fprintf(stderr, "libwebsockets is not compiled with UNIX domain socket support");
            return -1;
#endif
        }
    }
#endif
#if HAVE_OPENSSL
    if (opts.ssl) {
        info.ssl_cert_filepath = opts.cert_path;
        info.ssl_private_key_filepath = opts.key_path;
        info.ssl_ca_filepath = opts.ca_path;
        info.ssl_cipher_list = "ECDHE-ECDSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES256-GCM-SHA384:"
                "DHE-RSA-AES256-GCM-SHA384:"
                "ECDHE-RSA-AES256-SHA384:"
                "HIGH:!aNULL:!eNULL:!EXPORT:"
                "!DES:!MD5:!PSK:!RC4:!HMAC_SHA1:"
                "!SHA1:!DHE-RSA-AES128-GCM-SHA256:"
                "!DHE-RSA-AES128-SHA256:"
                "!AES128-GCM-SHA256:"
                "!AES128-SHA256:"
                "!DHE-RSA-AES256-SHA256:"
                "!AES256-GCM-SHA384:"
                "!AES256-SHA256";
        if (info.ssl_ca_filepath != NULL && strlen(info.ssl_ca_filepath) > 0)
            info.options |= LWS_SERVER_OPTION_REQUIRE_VALID_OPENSSL_CLIENT_CERT;
#if LWS_LIBRARY_VERSION_MAJOR >= 2
        info.options |= LWS_SERVER_OPTION_REDIRECT_HTTP_TO_HTTPS;
#endif
    }
#endif

    context = lws_create_context(&info);
    if (context == NULL) {
        lwsl_err("libwebsockets init failed\n");
        return 1;
    }
    vhost = lws_create_vhost(context, &info);
#if LWS_LIBRARY_VERSION_MAJOR >= 3
    http_port = lws_get_vhost_port(vhost);
#else
    http_port = info.port;
#endif

    char *cname = make_socket_name(false);
    backend_socket_name = cname;
    lwsl_notice("creating server socket: '%s'\n", cname);
    lws_sock_file_fd_type csocket;
    csocket.filefd = create_command_socket(cname);
    cmdwsi = lws_adopt_descriptor_vhost(vhost, LWS_ADOPT_RAW_FILE_DESC,
                                        csocket, "cmd", NULL);
    cclient = (struct cmd_client *) lws_wsi_user(cmdwsi);
    cclient->socket = csocket.filefd;
    make_html_file(http_port);

    lwsl_info("TTY configuration:\n");
    if (opts.credential != NULL)
        lwsl_info("  credential: %s\n", opts.credential);
    lwsl_info("  reconnect timeout: %ds\n", opts.reconnect);
    lwsl_info("  close signal: %s (%d)\n",
                opts.sig_name != NULL ? opts.sig_name :
                opts.sig_code == SIGHUP ? "SIGHUP" : "???",
                opts.sig_code);
    if (opts.check_origin)
        lwsl_info("  check origin: true\n");
    if (opts.readonly)
        lwsl_info("  readonly: true\n");
    if (opts.once)
        lwsl_info("  once: true\n");
    int ret;
    if (port_specified >= 0 && server->options.browser_command == NULL) {
        fprintf(stderr, "Server start on port %d. You can browse %s://localhost:%d/\n",
                http_port, opts.ssl ? "https" : "http", http_port);
        opts.http_server = true;
        ret = 0;
    } else {
        opts.cwd = getcwd(NULL, 0);
        opts.env = copy_strings((const char*const*) environ);
        ret = handle_command(argc-optind, (arglist_t)argv+optind, NULL, &opts);
        if (ret == EXIT_FAILURE)
            exit(ret);
    }

    if (ret == 0)
        maybe_daemonize();
    watch_settings_file();

    // libwebsockets main loop
    while (!force_exit) {
        lws_service(context, 100);
    }

    lws_context_destroy(context);

    // cleanup
    tty_server_free(server);

    return ret;
}
bool
is_WindowsSubsystemForLinux()
{
    static int is_WSL_cache;
    if (is_WSL_cache)
        return is_WSL_cache > 0;
    int r;
    int f = open("/proc/version", O_RDONLY);
    if (f < 0)
        r = false;
    else {
        char buf[512];
        int i = 0;
        for (;;) {
            size_t avail = sizeof(buf) - 1 - i;
            if (avail <= 0)
                break;
            ssize_t n = read(f, buf+i, avail);
            if (n <= 0)
                break;
            i += n;
        }
        buf[i] = '\0';
        r = strstr(buf, "Microsoft") != NULL;
        close(f);
    }
    is_WSL_cache = r ? 1 : -1;
    return r;
}

static char *userprofile_cache;

char *get_WSL_userprofile()
{
    if (userprofile_cache == NULL) {
        FILE *f = popen("/mnt/c/Windows/System32/cmd.exe /c \"<nul set /p=%UserProfile%\" 2>/dev/null", "r");
        if (f == NULL)
            return NULL;
        char buf[512];
        int i = 0;
        for (;;) {
            size_t avail = sizeof(buf) - 1 - i;
            if (avail <= 0)
                return NULL;
            size_t n = fread(buf+i, 1, avail, f);
            if (n == 0)
                break;
            i += n;
        }
        fclose(f);
        buf[i] = '\0';
        char *nl = strchr(buf, '\n');
        if (nl)
            *nl = '\0';
        char *cr = strchr(buf, '\r');
        if (cr)
            *cr = '\0';
        userprofile_cache = strdup(buf);
    }
    return userprofile_cache;
}

static const char *
domterm_dir(bool settings, bool check_wsl)
{
    char *user_profile;
    const char *user_prefix = "C:\\Users\\";
    size_t user_prefix_length;
    char *tmp;
    char *xdg_home = getenv(settings ? "XDG_CONFIG_HOME" : "XDG_RUNTIME_DIR");
    const char *sini = settings ? "/settings.ini" : "";
    if (xdg_home) {
	tmp = challoc(strlen(xdg_home)+40);
	sprintf(tmp, "%s/domterm%s", xdg_home, sini);
    } else if (check_wsl && is_WindowsSubsystemForLinux()
	&& (user_profile = get_WSL_userprofile()) != NULL
	&& strlen(user_profile) > (user_prefix_length = strlen(user_prefix))
	&& memcmp(user_profile, user_prefix, user_prefix_length) == 0) {
	const char *fmt = "/mnt/c/Users/%s/AppData/%s/DomTerm%s";
	const char *subdir = settings ? "Roaming" : "Local";
	tmp = challoc(strlen(fmt) + strlen(user_profile) + 40);
	sprintf(tmp, fmt, user_profile+user_prefix_length, subdir, sini);
    } else {
        const char *home = find_home();
        tmp = challoc(strlen(home)+30);
        sprintf(tmp, "%s/.domterm%s", home, sini);
        if (settings && access(tmp, R_OK) != 0)
            sprintf(tmp, "%s/.config/domterm%s", home, sini);
    }
    if (! settings && mkdir(tmp, S_IRWXU) != 0 && errno != EEXIST)
        fatal("cannot create directory '%s'", tmp);
    return tmp;
}

const char *
domterm_settings_default()
{
    static const char *dir = NULL;
    if (dir == NULL)
      dir = domterm_dir(true, true);
    return dir;
}

const char *
domterm_socket_dir()
{
    static const char *dir = NULL;
    if (dir == NULL)
      dir = domterm_dir(false, false);
    return dir;
}

const char *
domterm_genhtml_dir()
{
    static const char *dir = NULL;
    if (dir == NULL)
      dir = domterm_dir(false, true);
    return dir;
}

static char *
make_socket_name(bool html_filename)
{
    const char *ddir = html_filename ? domterm_genhtml_dir()
        : domterm_socket_dir();
    char *r;
    char *socket_name = opts.socket_name;
    if (socket_name != NULL && socket_name[0] != 0) {
        int dot = -1;
        for (int i = 0; ; i++) {
            char ch = socket_name[i];
            if (ch == 0)
              break;
            if (ch == '.')
              dot = i;
            if (ch == '/')
              dot = -1;
        }
	int socket_name_length = strlen(socket_name);
        const char *ext;
	if (html_filename) {
            ext = dot < 0 ? ".html" : "";
	    if (dot >= 0)
                socket_name_length = dot;
	} else
            ext = dot < 0 ? ".socket" : "";
        int len = socket_name_length + strlen(ext);
        if (socket_name[0] != '/') {
            r = challoc(len + strlen(ddir) + 2);
            sprintf(r, "%s/%.*s%s", ddir, socket_name_length, socket_name, ext);
        } else {
            r = challoc(len + 1);
            sprintf(r, "%.*s%s", socket_name_length, socket_name, ext);
        }
    } else {
      const char *sname = html_filename ? "/default.html" : "/default.socket";
        r = challoc(strlen(ddir)+strlen(sname)+1);
        sprintf(r, "%s%s", ddir, sname);
    }
    return r;
}

char server_key[SERVER_KEY_LENGTH];

char *main_html_url;
char *main_html_path;

static const char * standard_stylesheets[] = {
    "hlib/domterm-core.css",
    "hlib/domterm-standard.css",
    "hlib/goldenlayout-base.css",
    "hlib/jsMenus.css",
    "hlib/domterm-layout.css",
    "hlib/domterm-default.css",
#if WITH_XTERMJS
    "hlib/xterm.css",
#endif
    NULL
};
static const char * standard_stylesheets_simple[] = {
    "hlib/domterm-core.css",
    "hlib/domterm-standard.css",
    "hlib/domterm-default.css",
#if WITH_XTERMJS
    "hlib/xterm.css",
#endif
    NULL
};
struct lib_info {
    const char *file;
    int options;
};

static struct lib_info standard_jslibs[] = {
    {"hlib/domterm.js", LIB_WHEN_OUTER|LIB_WHEN_SIMPLE},
    {"hlib/domterm-version.js", LIB_WHEN_OUTER|LIB_WHEN_SIMPLE},
#if COMBINE_RESOURCES
    {"hlib/dt-combined.js", LIB_WHEN_SIMPLE},
#else
    {"hlib/terminal.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
#if ! WITH_XTERMJS
    {"hlib/domterm-parser.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
    {"hlib/sixel/Colors.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
    {"hlib/sixel/SixelDecoder.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
#endif
    {"hlib/browserkeymap.js", LIB_WHEN_SIMPLE},
    {"hlib/commands.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
    {"hlib/wcwidth.js", LIB_WHEN_SIMPLE},
    {"hlib/mark.es6.js", LIB_WHEN_SIMPLE},
    {"hlib/domterm-findtext.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
    {"hlib/FileSaver.js", LIB_WHEN_SIMPLE},
    {"hlib/ResizeSensor.js", LIB_WHEN_SIMPLE},
#endif
#if COMBINE_RESOURCES
    {"hlib/dt-outer.js", LIB_WHEN_OUTER},
#else
    {"hlib/jquery.min.js", LIB_WHEN_OUTER},
    {"hlib/goldenlayout.js", LIB_WHEN_OUTER},
    {"hlib/domterm-layout.js", LIB_WHEN_OUTER},
    {"hlib/domterm-menus.js", LIB_WHEN_OUTER},
    {"hlib/qwebchannel.js", LIB_WHEN_OUTER|LIB_WHEN_SIMPLE},
    {"hlib/jsMenus.js", LIB_WHEN_OUTER},
    {"hlib/screenfull.js", LIB_WHEN_OUTER},
#endif
    {"hlib/domterm-client.js", LIB_WHEN_OUTER|LIB_WHEN_SIMPLE},
#if WITH_XTERMJS
    {"hlib/xterm.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
    {"hlib/fit.js", LIB_WHEN_SIMPLE|LIB_AS_MODULE},
#endif
    {NULL, 0},
};

/* The iniial URL passed to the browser is a file: URL to a user-read-only file.
 * This verifies that the browser is running as the current user.
 * It is just a stub file that redirects to a http URL with the real resources.
 * We do this because CORS (Cross-Origin Resource Sharing) restrictions
 * are incompatible with file URLs, at least when using a desktop browser.
 */

static void
make_main_html_text(struct sbuf *obuf, int port)
{
    const char *start_html = "no-frames.html";
    sbuf_printf(obuf,
                "<!DOCTYPE html>\n"
                "<html><head>\n"
                "<meta http-equiv='Content-Type' content='text/html;"
                " charset=UTF-8'>\n"
                "<meta http-equiv=\"Content-Security-Policy\" content=\"script-src 'unsafe-inline'\">\n"
                "<script type='text/javascript'>\n"
                "var DomTerm_server_key = '%.*s';\n"
                "var newloc = 'http://localhost:%d/%s' + location.hash;\n"
                "newloc += (newloc.includes('#') ? ';' : '#')+'server-key=' + DomTerm_server_key;\n"
                "location.replace(newloc);\n"
                "</script>\n"
                "</head>\n"
                "<body></body></html>\n",
                SERVER_KEY_LENGTH, server_key, port, start_html
        );
    lwsl_notice("initial html redirects to: 'http://localhost:%d/%s'\n", port, start_html);
}
void
make_html_text(struct sbuf *obuf, int port, int hoptions,
               const char *body_text, int body_length)
{
    char base[40];
    bool simple = (hoptions & LIB_WHEN_OUTER) == 0;
    sprintf(base, "http://%s:%d/", "localhost", port);
    sbuf_printf(obuf,
                "<!DOCTYPE html>\n"
                "<html><head>\n"
                "<base href='%s'/>\n"
                "<meta http-equiv='Content-Type' content='text/html;"
                " charset=UTF-8'>\n"
                "<title>DomTerm</title>\n",
                base);
    const char **p;
    for (p = simple ? standard_stylesheets_simple : standard_stylesheets; *p; p++) {
        sbuf_printf(obuf,
                    "<link type='text/css' rel='stylesheet' href='%s'>\n",
                    *p);
    }
    struct lib_info *lib;
    for (lib = standard_jslibs; lib->file != NULL; lib++) {
        const char *jstype = (lib->options & LIB_AS_MODULE) ? "module" : "text/javascript";
        if ((hoptions & lib->options & (LIB_WHEN_SIMPLE|LIB_WHEN_OUTER)) != 0)
            sbuf_printf(obuf,
                        "<script type='%s' src='%s'> </script>\n",
                        jstype, lib->file);
    }
    if ((hoptions & LIB_WHEN_OUTER) != 0)
        sbuf_printf(obuf,
                    "<script type='text/javascript'>\n"
                    "DomTerm.server_port = %d;\n"
                    "DomTerm.server_key = '%.*s';\n"
                    "</script>\n",
                    port, SERVER_KEY_LENGTH, server_key);
    sbuf_printf(obuf,
                "</head>\n"
                "<body>%.*s</body>\n"
                "</html>\n", body_length, body_text);
}

static void
make_html_file(int port)
{
    //uid_t uid = getuid();
    char *sname = make_socket_name(true);
    char *sext = strrchr(sname, '.');
    const char*prefix = "file://";
    const char *ext = ".html";
    char *buf = challoc(strlen(prefix)+(sext-sname)+strlen(ext)+1);
    sprintf(buf, "%s%.*s%s", prefix, (int) (sext-sname), sname, ext);
    main_html_url = buf;
    main_html_path = buf+strlen(prefix);
    if (server_key[0] == 0)
        generate_random_string(server_key, SERVER_KEY_LENGTH);
    lwsl_notice("initial html file: '%s'\n", main_html_path);
    struct sbuf obuf[1];
    sbuf_init(obuf);
    make_main_html_text(obuf, port);

    int hfile = open(main_html_path, O_WRONLY|O_CREAT|O_TRUNC, S_IRWXU);
    if (hfile < 0
        || write(hfile, obuf->buffer, obuf->len) != obuf->len
        || close(hfile) != 0)
        lwsl_err("writing %s failed\n", main_html_path);
    sbuf_free(obuf);
}

void
error(const char *format, ...)
{
    va_list args;
    va_start (args, format);
    vfprintf (stderr, format, args);
    va_end (args);
    fprintf (stderr, "\n");
}

void
fatal(const char *format, ...)
{
    va_list args;
    va_start (args, format);
    vfprintf (stderr, format, args);
    va_end (args);
    fprintf (stderr, "\n");
    exit(-1);
}

#if 0
/*
 * Get server create lock. If already held then server start is happening in
 * another client, so block until the lock is released and return -2 to
 * retry. Return -1 on failure to continue and start the server anyway.
 */
static int
client_get_lock(char *lockfile)
{
        int lockfd;

        lwsl_notice("lock file is %s\n", lockfile);

        if ((lockfd = open(lockfile, O_WRONLY|O_CREAT, 0600)) == -1) {
                lwsl_notice("open failed: %s\n", strerror(errno));
                return (-1);
        }

        if (flock(lockfd, LOCK_EX|LOCK_NB) == -1) {
                lwsl_notice("flock failed: %s\n", strerror(errno));
                if (errno != EAGAIN)
                        return (lockfd);
                while (flock(lockfd, LOCK_EX) == -1 && errno == EINTR)
                        /* nothing */;
                close(lockfd);
                return (-2);
        }
        lwsl_notice("flock succeeded\n");

        return (lockfd);
}
#endif
