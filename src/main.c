#define _GNU_SOURCE
#include "state.h"
#include "api.h"
#include "scanner.h"
#include "watch.h"
#include "procutil.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

app_state_t g_state;

static volatile int g_running = 1;

static void sig_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static void write_pidfile(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)getpid());
    fclose(f);
}

static void redirect_logs(const char *path) {
    if (!path || !*path) return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
}

/* Declared in api.c */
void api_ws_broadcast_timer(void *arg);

int main(int argc, char *argv[]) {
    memset(&g_state, 0, sizeof(g_state));

    /* defaults */
    g_state.port = 8080;
    strncpy(g_state.www_path,  "./www",              sizeof(g_state.www_path)  - 1);
    strncpy(g_state.pidfile,   "/tmp/memcheat.pid",  sizeof(g_state.pidfile)   - 1);
    strncpy(g_state.watchfile, "/tmp/watchlist.json",sizeof(g_state.watchfile) - 1);
    strncpy(g_state.cheatdir,  "/tmp",               sizeof(g_state.cheatdir)  - 1);
    char logfile[512] = "";

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--port")      && i+1 < argc) g_state.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--www")       && i+1 < argc) strncpy(g_state.www_path,  argv[++i], sizeof(g_state.www_path)  - 1);
        else if (!strcmp(argv[i], "--pidfile")   && i+1 < argc) strncpy(g_state.pidfile,   argv[++i], sizeof(g_state.pidfile)   - 1);
        else if (!strcmp(argv[i], "--logfile")   && i+1 < argc) strncpy(logfile,            argv[++i], sizeof(logfile)           - 1);
        else if (!strcmp(argv[i], "--watchfile") && i+1 < argc) strncpy(g_state.watchfile, argv[++i], sizeof(g_state.watchfile) - 1);
        else if (!strcmp(argv[i], "--cheatdir")  && i+1 < argc) strncpy(g_state.cheatdir,  argv[++i], sizeof(g_state.cheatdir)  - 1);
    }

    redirect_logs(logfile);

    /* close stdin */
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) { dup2(nullfd, STDIN_FILENO); close(nullfd); }

    write_pidfile(g_state.pidfile);

    pthread_mutex_init(&g_state.lock, NULL);
    scanner_init(&g_state.scanner);
    watch_init(&g_state.watchlist);
    watch_load(&g_state.watchlist, g_state.watchfile);
    watch_start_freeze_thread(&g_state.watchlist);

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);
    signal(SIGPIPE, SIG_IGN);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    char listen_url[64];
    snprintf(listen_url, sizeof(listen_url), "http://0.0.0.0:%d", g_state.port);

    if (!mg_http_listen(&mgr, listen_url, api_handle, NULL)) {
        fprintf(stderr, "Failed to listen on %s\n", listen_url);
        return 1;
    }

    mg_timer_add(&mgr, 500, MG_TIMER_REPEAT, api_ws_broadcast_timer, &mgr);

    printf("memcheat listening on %s\n", listen_url);
    fflush(stdout);

    while (g_running) {
        mg_mgr_poll(&mgr, 100);
    }

    printf("memcheat shutting down\n");
    watch_save(&g_state.watchlist, g_state.watchfile);
    watch_stop(&g_state.watchlist);
    mg_mgr_free(&mgr);
    pthread_mutex_destroy(&g_state.lock);
    remove(g_state.pidfile);

    return 0;
}
