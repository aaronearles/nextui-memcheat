#define _GNU_SOURCE
#include "watch.h"
#include "scanner.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void *freeze_thread_fn(void *arg) {
    watch_list_t *wl = (watch_list_t *)arg;
    while (wl->running) {
        usleep(100000); /* 100 ms */

        pthread_mutex_lock(&g_state.lock);
        if (g_state.scanner.pid > 0) {
            for (int i = 0; i < wl->count; i++) {
                watch_entry_t *e = &wl->entries[i];
                if (e->frozen) {
                    scanner_write(&g_state.scanner,
                                  e->addr, e->width, e->freeze_value);
                }
            }
        }
        pthread_mutex_unlock(&g_state.lock);
    }
    return NULL;
}

void watch_init(watch_list_t *wl) {
    memset(wl, 0, sizeof(*wl));
}

void watch_start_freeze_thread(watch_list_t *wl) {
    wl->running = 1;
    pthread_create(&wl->freeze_thread, NULL, freeze_thread_fn, wl);
}

void watch_stop(watch_list_t *wl) {
    wl->running = 0;
    pthread_join(wl->freeze_thread, NULL);
}

int watch_find(watch_list_t *wl, uint64_t addr) {
    for (int i = 0; i < wl->count; i++) {
        if (wl->entries[i].addr == addr) return i;
    }
    return -1;
}

int watch_add(watch_list_t *wl, uint64_t addr, const char *label, int width) {
    if (wl->count >= MAX_WATCH_ENTRIES) return -1;
    if (watch_find(wl, addr) >= 0) return -2; /* already exists */

    watch_entry_t *e = &wl->entries[wl->count];
    e->addr         = addr;
    e->width        = width;
    e->frozen       = 0;
    e->freeze_value = 0;
    strncpy(e->label, label ? label : "", MAX_LABEL_LEN - 1);
    e->label[MAX_LABEL_LEN - 1] = '\0';
    wl->count++;
    return 0;
}

int watch_remove(watch_list_t *wl, uint64_t addr) {
    int i = watch_find(wl, addr);
    if (i < 0) return -1;
    /* shift entries down */
    for (int j = i; j < wl->count - 1; j++)
        wl->entries[j] = wl->entries[j + 1];
    wl->count--;
    return 0;
}

int watch_set_label(watch_list_t *wl, uint64_t addr, const char *label) {
    int i = watch_find(wl, addr);
    if (i < 0) return -1;
    strncpy(wl->entries[i].label, label, MAX_LABEL_LEN - 1);
    wl->entries[i].label[MAX_LABEL_LEN - 1] = '\0';
    return 0;
}

int watch_set_freeze(watch_list_t *wl, uint64_t addr, int enabled, uint64_t value) {
    int i = watch_find(wl, addr);
    if (i < 0) return -1;
    wl->entries[i].frozen       = enabled;
    wl->entries[i].freeze_value = value;
    return 0;
}

int watch_load(watch_list_t *wl, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    wl->count = 0;
    char line[512];
    watch_entry_t e;
    char label[MAX_LABEL_LEN];

    while (fgets(line, sizeof(line), f) && wl->count < MAX_WATCH_ENTRIES) {
        memset(&e, 0, sizeof(e));
        label[0] = '\0';
        /* format: addr width frozen freeze_value "label" */
        unsigned long long addr, fval;
        int frozen, width;
        int n = sscanf(line, "%llx %d %d %llx \"%63[^\"]\"",
                       &addr, &width, &frozen, &fval, label);
        if (n < 4) continue;
        e.addr         = addr;
        e.width        = width;
        e.frozen       = frozen;
        e.freeze_value = fval;
        strncpy(e.label, label, MAX_LABEL_LEN - 1);
        e.label[MAX_LABEL_LEN - 1] = '\0';
        wl->entries[wl->count++] = e;
    }

    fclose(f);
    return 0;
}

int watch_save(watch_list_t *wl, const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    for (int i = 0; i < wl->count; i++) {
        watch_entry_t *e = &wl->entries[i];
        fprintf(f, "%llx %d %d %llx \"%s\"\n",
                (unsigned long long)e->addr,
                e->width,
                e->frozen,
                (unsigned long long)e->freeze_value,
                e->label);
    }

    fclose(f);
    return 0;
}
