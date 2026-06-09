#pragma once
#include "scanner.h"
#include "watch.h"
#include <pthread.h>

typedef struct {
    scanner_t      scanner;
    watch_list_t   watchlist;
    char           www_path[512];
    char           watchfile[512];
    char           cheatdir[512];
    char           pidfile[512];
    int            port;
    pthread_mutex_t lock;
    int            scan_running; /* 1 while background scan thread is active */
    int            scan_done;    /* set to 1 by scan thread; cleared by timer after broadcast */
} app_state_t;

extern app_state_t g_state;
