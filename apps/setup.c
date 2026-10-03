/*
 * slop-setup -- the SlopOS first-run wizard.
 *
 * Runs the first time the machine boots (init launches it when there is no
 * /home/user/.config/slop/.configured marker) and walks the user through a
 * handful of questions: language, keyboard, timezone, the account name, the
 * look of the desktop, and which optional apps to install.
 *
 * The optional apps are real: "Install Zen Browser" and "Install OBS Studio"
 * run /usr/bin/slop-install, which unpacks the package bundled on the image
 * with busybox. Because the download is already on the disc, installation is
 * a local unpack -- no network is needed.
 *
 * On the last page it writes /home/user/.config/slop/setup.conf and the
 * .configured marker so the shell boots straight to the desktop next time.
 */
#define _GNU_SOURCE
#include "../libslop/slop.h"
#include "../libslop/slop_icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

static const char *CONF_DIR = "/home/user/.config/slop";
static const char *CONF     = "/home/user/.config/slop/setup.conf";
static const char *MARKER   = "/home/user/.config/slop/.configured";

/* ------------------------------------------------------------- config */
static const char *LANGUAGES[] = { "English", "Deutsch", "Français", "Español" };
static const char *KEYMAPS[]   = { "us", "de", "fr", "es" };
static const char *TIMEZONES[] = { "UTC", "Europe/London", "Europe/Berlin", "US/Eastern" };

static int    lang = 0, keymap = 0, tz = 1;
static char   user[32] = "user";
static int    custom_host = 0;       /* "slopos" vs a custom name */
static char   host[32] = "slopos";
static int    accent = 0, scale = 1;

/* install progress: 0 idle, 1 installing, 2 done, 3 failed */
static int    zen_state = 0, obs_state = 0, res_state = 0;
static int    install_pid = 0;
static const char *installing = NULL;

/* ------------------------------------------------------------- helpers */
static void read_status(char *out, int cap) {
    int fd = open("/run/slop/install.status", O_RDONLY);
    if (fd < 0) { out[0] = 0; return; }
    int n = read(fd, out, cap - 1);
    close(fd);
    out[n > 0 ? n : 0] = 0;
    for (char *p = out; *p; p++) if (*p == '\n' || *p == '\r') { *p = 0; break; }
}

static int installed(const char *exe) { return access(exe, X_OK) == 0; }

/* Start installing one package in the background. */
static void begin_install(const char *id) {
    if (install_pid) return;
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/usr/bin/slop-install", "slop-install", id, (char *)NULL);
        execl("/bin/slop-install", "slop-install", id, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) { install_pid = (int)pid; installing = id; }
}

/* Reap a finished installer and fold the result into the app states. */
static void poll_install(void) {
    if (!install_pid) return;
    int st = 0;
    if (waitpid(install_pid, &st, WNOHANG) != install_pid) return;
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    if (installing && !strcmp(installing, "zen"))
        zen_state = ok ? 2 : 3;
    else if (installing && !strcmp(installing, "obs"))
        obs_state = ok ? 2 : 3;
    install_pid = 0;
    installing = NULL;
}

static void write_config(void) {
    mkdir("/home/user/.config", 0755);
    mkdir(CONF_DIR, 0755);
    FILE *f = fopen(CONF, "w");
    if (f) {
        fprintf(f,
            "# SlopOS setup -- written by slop-setup\n"
            "language=%s\nkeymap=%s\ntimezone=%s\n"
            "username=%s\nhostname=%s\n"
            "accent=%d\nscale=%d\n"
            "install_zen=%d\ninstall_obs=%d\ninstall_resolve=%d\n",
            LANGUAGES[lang], KEYMAPS[keymap], TIMEZONES[tz],
            user, custom_host ? host : "slopos", accent, scale,
            zen_state == 2, obs_state == 2, res_state == 2);
        fclose(f);
    }
    int m = open(MARKER, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (m >= 0) { write(m, "1\n", 2); close(m); }
}

/* ------------------------------------------------------------- drawing */
static void draw_header(void) {
    slop_vgradient(0, 0, slop.w, 92, SLOP_RGB(0x22, 0x28, 0x3a), slop_theme_dark.bg);
    slop_text(&slop_font_title, 28, 44, "Welcome to SlopOS", slop_theme_dark.text);
    slop_text(&slop_font_body, 28, 70, "\"Aurora\"  --  let's set up your machine",
              slop_theme_dark.text_dim);
}

/* Step dots across the header. */
static void draw_steps(int page, int npages) {
    int n = npages;
    int total = n * 18;
    int x = slop.w - 28 - total;
    for (int i = 0; i < n; i++) {
        slop_color c = (i == page) ? slop_theme_dark.accent_hi
                     : (i < page)  ? slop_theme_dark.accent_lo
                                   : slop_theme_dark.border;
        slop_fill_circle(x + i * 18 + 5, 40, 5, c);
    }
}

/* A labelled row with a segmented control, used for several questions. */
static int question_seg(const char *title, const char *sub, int y,
                        const char *const *opts, int n, int *sel) {
    slop_card(24, y, slop.w - 48, 66, title, sub);
    return slop_segmented(24 + 20, y + 34, slop.w - 48 - 40, 26, opts, n, sel);
}

/* One installable app row. Returns 1 if its button was pressed. */
static int draw_app_row(int y, int icon, const char *name, const char *desc,
                        const char *size, int state, const char *exe, int busy) {
    int w = slop.w - 48;
    int hot = ui.mx >= 24 && ui.mx < 24 + w && ui.my >= y && ui.my < y + 72;
    slop_fill_round(24, y, w, 72, 12,
                    hot ? slop_theme_dark.surface_hi : slop_theme_dark.surface);
    slop_round_outline(24, y, w, 72, 12, 1, slop_theme_dark.border);
    slop_icon_draw(icon, 40, y + 20, 32, 0);
    slop_text(&slop_font_bold, 86, y + 30, name, slop_theme_dark.text);
    slop_text(&slop_font_small, 86, y + 52, desc, slop_theme_dark.text_dim);

    int bx = 24 + w - 168;
    if (state == 2 || installed(exe)) {
        slop_badge(bx + 40, y + 46, "Installed", slop_theme_dark.good);
    } else if (busy) {
        slop_text(&slop_font_small, bx + 20, y + 40, "Please wait",
                  slop_theme_dark.text_mute);
    } else if (state == 1) {
        slop_spinner(bx + 40, y + 36, 10, slop_theme_dark.accent_hi);
        slop_text(&slop_font_small, bx + 58, y + 40, "Installing...",
                  slop_theme_dark.accent_hi);
    } else if (state == 3) {
        slop_badge(bx + 40, y + 46, "Failed", slop_theme_dark.bad);
    } else {
        slop_button b = { bx, y + 20, 148, 32, "Install", 0, 1, 0, 0 };
        if (slop_button_draw(&b)) return 1;
        slop_text(&slop_font_small, 86, y + 66, size, slop_theme_dark.text_mute);
    }
    return 0;
}

static void nav_buttons(int page, int *back, int *next, const char *next_label) {
    int y = slop.h - 56;
    slop_button b = { 24, y, 110, 36, "Back", 0, page > 0, 0, 0 };
    *back = slop_button_draw(&b);
    int nw = 170;
    slop_button n = { slop.w - 24 - nw, y, nw, 36,
                      next_label ? next_label : "Continue", 1, 1, 0, 0 };
    *next = slop_button_draw(&n);
}

int main(void) {
    if (slop_app_start("SlopOS Setup", SLOP_ICON_SETTINGS) != 0) return 1;
    slop_flush_events();

    int page = 0, cursor = 0, focused = 0;
    const int NPAGES = 5;

    for (;;) {
        slop_begin_frame();
        if (ui.quit) break;
        slop_clear(slop_theme_dark.bg);
        slop_chrome c = slop_window("SlopOS Setup", "first-run configuration", 0);

        /* The chrome gives us a content rectangle; draw in local coordinates
           by offsetting. libslop's window origin is the content area top-left,
           so we simply use slop.w/slop.h. */
        draw_header();
        draw_steps(page, NPAGES);

        int y = 112;
        if (page == 0) {
            slop_text(&slop_font_bold, 24, y, "Language & keyboard", slop_theme_dark.text);
            y += 14;
            question_seg("Language", "the language of the interface", y,
                         LANGUAGES, 4, &lang);
            y += 82;
            slop_card(24, y, slop.w - 48, 66, "Keyboard layout",
                      "how keys are mapped to characters");
            slop_segmented(24 + 20, y + 34, slop.w - 48 - 40, 26, KEYMAPS, 4, &keymap);
            y += 82;
            slop_text(&slop_font_small, 24, y, "You can change both later in Settings.",
                      slop_theme_dark.text_mute);
        } else if (page == 1) {
            slop_text(&slop_font_bold, 24, y, "Where are you?", slop_theme_dark.text);
            y += 14;
            slop_card(24, y, slop.w - 48, 66, "Timezone", "sets the desktop clock");
            slop_segmented(24 + 20, y + 34, slop.w - 48 - 40, 26, TIMEZONES, 4, &tz);
            y += 86;
            slop_card(24, y, slop.w - 48, 66, "Keep my location private",
                      "SlopOS never sends it anywhere");
            static int priv = 1;
            slop_toggle(24 + slop.w - 48 - 96, y + 16, &priv);
        } else if (page == 2) {
            slop_text(&slop_font_bold, 24, y, "Your account", slop_theme_dark.text);
            y += 16;
            slop_text(&slop_font_small, 24, y, "Account name", slop_theme_dark.text_dim);
            y += 12;
            slop_textfield(24, y, slop.w - 48, 38, user, sizeof(user), &cursor, &focused,
                           "user");
            y += 58;
            slop_card(24, y, slop.w - 48, 66, "Custom computer name",
                      "otherwise the machine is called slopos");
            slop_toggle(24 + slop.w - 48 - 96, y + 16, &custom_host);
            y += 78;
            if (custom_host)
                slop_textfield(24, y, slop.w - 48, 36, host, sizeof(host), &cursor,
                               &focused, "slopos");
        } else if (page == 3) {
            slop_text(&slop_font_bold, 24, y, "Make it yours", slop_theme_dark.text);
            y += 16;
            question_seg("Accent colour", "used across the whole desktop", y,
                         (const char *const[]){ "Blue", "Sunset", "Mint", "Graphite" },
                         4, &accent);
            y += 86;
            slop_card(24, y, slop.w - 48, 66, "Display scaling",
                      "1x looks best on a normal screen");
            static const char *SCALES[] = { "1x", "1.25x", "1.5x", "2x" };
            slop_segmented(24 + 20, y + 34, slop.w - 48 - 40, 26, SCALES, 4, &scale);
        } else if (page == 4) {
            poll_install();
            slop_text(&slop_font_bold, 24, y, "Install apps", slop_theme_dark.text);
            slop_text(&slop_font_small, 24 + slop_text_w(&slop_font_bold, "Install apps") + 12,
                      y, "bundled on this disc -- no internet needed", slop_theme_dark.text_mute);
            y += 16;

            /* keep states in sync with reality */
            if (zen_state == 0 && installed("/opt/zen/zen")) zen_state = 2;
            if (obs_state == 0 && installed("/opt/obs/obs")) obs_state = 2;

            int busy = install_pid != 0;
            if (draw_app_row(y, SLOP_ICON_ZEN, "Zen Browser",
                             "Fast, private web browsing", "~120 MB",
                             zen_state, "/opt/zen/zen", busy))
                begin_install("zen");
            y += 80;
            if (draw_app_row(y, SLOP_ICON_OBS, "OBS Studio",
                             "Record and stream your screen", "~300 MB",
                             obs_state, "/opt/obs/obs", busy))
                begin_install("obs");
            y += 80;
            if (draw_app_row(y, SLOP_ICON_RESOLVE, "DaVinci Resolve",
                             "Professional video editing (media runtime, installs later)",
                             "", res_state, "/opt/davinci/bin/resolve", busy))
                begin_install("davinci");

            if (install_pid) {
                char s[64];
                read_status(s, sizeof(s));
                int pct = 50;
                if (strstr(s, "done")) pct = 100;
                slop_text(&slop_font_small, 24, slop.h - 78,
                          installing ? "Installing, please wait..." : "Working...",
                          slop_theme_dark.accent_hi);
                slop_progress(24, slop.h - 68, slop.w - 48, 8, pct,
                              slop_theme_dark.accent);
            }
        }

        int back = 0, next = 0;
        if (page == 4) {
            const char *lbl = install_pid ? "Finishing..." : "Finish setup";
            nav_buttons(page, &back, &next, lbl);
            if (next && !install_pid) { write_config(); break; }
        } else {
            nav_buttons(page, &back, &next, NULL);
        }
        if (back && page > 0) page--;
        if (next && page < NPAGES - 1) page++;

        (void)c;
        slop_end_frame();
        usleep(8000);
    }

    write_config();
    slop_shutdown();
    return 0;
}
