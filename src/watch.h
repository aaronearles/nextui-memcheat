#pragma once
#include <stdint.h>
#include <pthread.h>

#define MAX_WATCH_ENTRIES 64
#define MAX_LABEL_LEN     64

typedef struct {
    uint64_t addr;
    char     label[MAX_LABEL_LEN];
    int      width;
    int      frozen;
    uint64_t freeze_value;
} watch_entry_t;

typedef struct {
    watch_entry_t  entries[MAX_WATCH_ENTRIES];
    int            count;
    pthread_t      freeze_thread;
    volatile int   running;
} watch_list_t;

void watch_init(watch_list_t *wl);
void watch_start_freeze_thread(watch_list_t *wl);
void watch_stop(watch_list_t *wl);
int  watch_add(watch_list_t *wl, uint64_t addr, const char *label, int width);
int  watch_remove(watch_list_t *wl, uint64_t addr);
int  watch_set_label(watch_list_t *wl, uint64_t addr, const char *label);
int  watch_set_freeze(watch_list_t *wl, uint64_t addr, int enabled, uint64_t value);
int  watch_find(watch_list_t *wl, uint64_t addr);
int  watch_load(watch_list_t *wl, const char *path);
int  watch_save(watch_list_t *wl, const char *path);
