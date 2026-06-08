#include "procutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <signal.h>
#include <ctype.h>

static const char *known_emulators[] = {
    "retroarch", "gambatte_libretro", "snes9x", "mgba",
    "pcsx_rearmed", "fbalpha2012", NULL
};

static int is_known_emulator(const char *name) {
    for (int i = 0; known_emulators[i]; i++) {
        if (strstr(name, known_emulators[i])) return 1;
    }
    return 0;
}

static int is_pid_dir(const char *name) {
    for (int i = 0; name[i]; i++) {
        if (!isdigit((unsigned char)name[i])) return 0;
    }
    return name[0] != '\0';
}

static void read_cmdline(pid_t pid, char *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return; }
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* cmdline uses NUL separators; replace first NUL with \0 to get argv[0] basename */
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\0') { buf[i] = '\0'; break; }
    }
}

int procutil_list(proc_info_t *results, int max) {
    DIR *dir = opendir("/proc");
    if (!dir) return 0;

    /* First pass: collect known emulators */
    int count = 0;
    proc_info_t *all = calloc((size_t)max * 2, sizeof(proc_info_t));
    if (!all) { closedir(dir); return 0; }
    int all_count = 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && all_count < max * 2) {
        if (!is_pid_dir(ent->d_name)) continue;
        pid_t pid = (pid_t)atoi(ent->d_name);
        if (pid <= 1) continue;

        char cmdline[256];
        read_cmdline(pid, cmdline, sizeof(cmdline));
        if (cmdline[0] == '\0') continue;

        /* Extract basename */
        char *base = strrchr(cmdline, '/');
        base = base ? base + 1 : cmdline;

        all[all_count].pid = pid;
        strncpy(all[all_count].name, base, MAX_PROC_NAME - 1);
        all[all_count].name[MAX_PROC_NAME - 1] = '\0';
        all_count++;
    }
    closedir(dir);

    /* Prefer known emulators; fall back to all if none found */
    for (int i = 0; i < all_count && count < max; i++) {
        if (is_known_emulator(all[i].name) ||
            strstr(all[i].name, ".pak")) {
            results[count++] = all[i];
        }
    }

    if (count == 0) {
        for (int i = 0; i < all_count && count < max; i++) {
            results[count++] = all[i];
        }
    }

    free(all);
    return count;
}

int procutil_name(pid_t pid, char *buf, size_t len) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) { buf[0] = '\0'; return -1; }
    size_t n = fread(buf, 1, len - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* strip trailing newline */
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    return 0;
}

int procutil_exists(pid_t pid) {
    return kill(pid, 0) == 0 ? 1 : 0;
}
