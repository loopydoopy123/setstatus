#define _POSIX_C_SOURCE 200809L
#include <alloca.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <alsa/asoundlib.h>
#include <X11/Xlib.h>

#define BAT_CAP_PATH  "/sys/class/power_supply/BAT0/capacity"

/* Read the first line of a file into buf (NUL-terminated, no trailing \n).
 * Returns length on success, -1 on error. */
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

/* Query the ALSA "Master" playback volume and mute state.
 * Writes e.g. "0.65" or "0.65 [MUTED]" into out.
 * Returns 0 on success, -1 on failure (out is set to ""). */
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

    /* Try channels in order until one works. */
    if (snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &volume) < 0 &&
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_MONO,       &volume) < 0 &&
        snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT,&volume) < 0)
        goto cleanup;

    if (max == min) goto cleanup;

    /* Round to nearest percent, then format as a 0.00..1.00 float like wpctl. */
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

int main(void) {
    char battery[16]  = "?";
    char vol[64]      = "";
    char datebuf[128] = "";
    char namebuf[512];

    /* 1. battery */
    if (read_line_file(BAT_CAP_PATH, battery, sizeof battery) < 0)
        strcpy(battery, "?");

    /* 2. volume via ALSA (no subprocess) */
    if (get_volume(vol, sizeof vol) < 0)
        vol[0] = '\0';

    /* 3. date (same format as the `date` command) */
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(datebuf, sizeof datebuf, "%a, %b %d, %H:%M:%S", &tm);

    /* 4. compose and set the root window name */
    snprintf(namebuf, sizeof namebuf,
             "Battery: %s%%        Volume: %s        %s  ",
             battery, vol, datebuf);

    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) {
        fprintf(stderr, "Cannot open X display\n");
        return 1;
    }
    XStoreName(dpy, DefaultRootWindow(dpy), namebuf);
    XFlush(dpy);
    XCloseDisplay(dpy);

    return 0;
}
