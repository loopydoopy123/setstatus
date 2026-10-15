#define _POSIX_C_SOURCE 200809L
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <alsa/asoundlib.h>
#include <X11/Xlib.h>

#define BAT_CAP_PATH    "/sys/class/power_supply/BAT0/capacity"
#define BAT_STAT_PATH   "/sys/class/power_supply/BAT0/status"

static volatile sig_atomic_t running = 1;
static void on_sig(int s) { (void)s; running = 0; }

/* ---- syscall-only file read ---- */
static ssize_t read_line_file(const char *path, char *buf, size_t bufsz) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    ssize_t n = read(fd, buf, bufsz - 1);
    int saved = errno;
    close(fd);
    errno = saved;

    if (n < 0) return -1;
    buf[n] = '\0';

    size_t len = (size_t)n;
    while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
        buf[--len] = '\0';
    return (ssize_t)len;
}

/* ---- ALSA volume query (no subprocess) ---- */
static int get_volume(char *out, size_t outsz) {
    snd_mixer_t *handle = NULL;
    snd_mixer_elem_t *elem;
    snd_mixer_selem_id_t *sid;
    long min = 0, max = 0, volume = 0;
    int muted = 0;
    int rc = -1;

    out[0] = '\0';

    if (snd_mixer_open(&handle, 0) < 0) return -1;
    if (snd_mixer_attach(handle, "default") < 0) goto cleanup;
    if (snd_mixer_selem_register(handle, NULL, NULL) < 0) goto cleanup;
    if (snd_mixer_load(handle) < 0) goto cleanup;

    snd_mixer_selem_id_alloca(&sid);
    snd_mixer_selem_id_set_index(sid, 0);
    snd_mixer_selem_id_set_name(sid, "Master");

    elem = snd_mixer_find_selem(handle, sid);
    if (!elem) goto cleanup;

    if (snd_mixer_selem_get_playback_volume_range(elem, &min, &max) < 0)
        goto cleanup;

    if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &volume) < 0 &&
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO,       &volume) < 0 &&
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT,&volume) < 0)
        goto cleanup;

    if (max == min) goto cleanup;

    long pct = ((volume - min) * 100 + (max - min) / 2) / (max - min);

    if (snd_mixer_selem_has_playback_switch(elem)) {
        int sw = 1;
        if (snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &sw) == 0)
            muted = !sw;
    }

    if (muted)
        snprintf(out, outsz, "%.2f [MUTED]", (double)pct / 100.0);
    else
        snprintf(out, outsz, "%.2f", (double)pct / 100.0);

    rc = 0;

cleanup:
    if (handle) snd_mixer_close(handle);
    return rc;
}

/* ---- fork/exec stdin-only helper for dmenu ---- */
static int spawn_with_stdin(char *const argv[], const char *input) {
    int pfd[2];
    if (pipe(pfd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]); close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        close(pfd[1]);
        if (dup2(pfd[0], STDIN_FILENO) < 0) _exit(127);
        close(pfd[0]);
        execvp(argv[0], argv);
        _exit(127);
    }

    close(pfd[0]);
    if (input && *input) {
        size_t len = strlen(input);
        ssize_t w = write(pfd[1], input, len);
        (void)w;
    }
    close(pfd[1]);

    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) { /* retry */ }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ---- sleep until next whole-second boundary ---- */
static void sleep_until_next_second(void) {
    struct timespec now, req;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0) { sleep(1); return; }
    req.tv_sec  = 0;
    req.tv_nsec = 1000000000L - now.tv_nsec;
    while (nanosleep(&req, &req) < 0 && errno == EINTR) { /* retry */ }
}

int main(void) {
    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);
    signal(SIGCHLD, SIG_DFL);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }

    char battery[16]  = "?";
    char status[32]   = "";
    char vol[64]      = "";
    char datebuf[128] = "";
    char namebuf[512];

    if (read_line_file(BAT_CAP_PATH, battery, sizeof battery) < 0)
        strcpy(battery, "?");

    char *dmenu_argv[] = {
        "dmenu", "-p", "LOW BATTERY",
        "-sb", "#B00000", "-nb", "#B00000",
        "-nf", "#B00000", "-sf", "#FFFFFF",
        NULL
    };

    while (running) {
        /* volume via ALSA */
        if (get_volume(vol, sizeof vol) < 0)
            vol[0] = '\0';

        /* date */
        time_t t = time(NULL);
        struct tm tm;
        localtime_r(&t, &tm);
        strftime(datebuf, sizeof datebuf, "%a, %b %d, %H:%M:%S", &tm);

        snprintf(namebuf, sizeof namebuf,
                 "Battery: %s%%        Volume: %s        %s  ",
                 battery, vol, datebuf);

        XStoreName(dpy, DefaultRootWindow(dpy), namebuf);
        XFlush(dpy);

        if (tm.tm_sec == 0) {
            if (read_line_file(BAT_CAP_PATH, battery, sizeof battery) < 0)
                strcpy(battery, "?");
            if (read_line_file(BAT_STAT_PATH, status, sizeof status) < 0)
                status[0] = '\0';

            int cap = atoi(battery);
            if (cap < 5 && strcmp(status, "Charging") != 0)
                spawn_with_stdin(dmenu_argv, "");

            sleep_until_next_second();
        }

        sleep_until_next_second();
    }

    XCloseDisplay(dpy);
    return 0;
}
