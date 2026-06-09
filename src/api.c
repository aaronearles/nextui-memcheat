#include "api.h"
#include "state.h"
#include "scanner.h"
#include "watch.h"
#include "procutil.h"
#include "cht.h"
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ── helpers ─────────────────────────────────────────────────────────────── */

static int starts_with(struct mg_str s, const char *prefix) {
    size_t n = strlen(prefix);
    return s.len >= n && strncmp(s.buf, prefix, n) == 0;
}

static int ends_with(struct mg_str s, const char *suffix) {
    size_t n = strlen(suffix);
    return s.len >= n && strncmp(s.buf + s.len - n, suffix, n) == 0;
}

static int uri_eq(struct mg_str uri, const char *path) {
    size_t n = strlen(path);
    return uri.len == n && strncmp(uri.buf, path, n) == 0;
}

static int is_method(struct mg_http_message *hm, const char *m) {
    size_t n = strlen(m);
    return hm->method.len == n && strncmp(hm->method.buf, m, n) == 0;
}

/* Extract address token between /api/watch/ and the next / or end */
static uint64_t extract_addr(struct mg_str uri) {
    const char *prefix = "/api/watch/";
    size_t plen = strlen(prefix);
    if (uri.len <= plen) return 0;
    const char *start = uri.buf + plen;
    size_t rem = uri.len - plen;
    const char *slash = memchr(start, '/', rem);
    size_t alen = slash ? (size_t)(slash - start) : rem;
    char buf[32] = "";
    if (alen >= sizeof(buf)) alen = sizeof(buf) - 1;
    memcpy(buf, start, alen);
    buf[alen] = '\0';
    return (uint64_t)strtoull(buf, NULL, 0);
}

/* mg_json_get_str in mongoose 7.14+ allocates; caller must free */
static int json_get_str(struct mg_str json, const char *path,
                        char *dst, size_t dstlen) {
    char *s = mg_json_get_str(json, path);
    if (!s) { dst[0] = '\0'; return 0; }
    strncpy(dst, s, dstlen - 1);
    dst[dstlen - 1] = '\0';
    free(s);
    return 1;
}

static int op_from_str(const char *s) {
    if (!strcmp(s, "eq"))        return SCAN_EQ;
    if (!strcmp(s, "lt"))        return SCAN_LT;
    if (!strcmp(s, "gt"))        return SCAN_GT;
    if (!strcmp(s, "changed"))   return SCAN_CHANGED;
    if (!strcmp(s, "unchanged")) return SCAN_UNCHANGED;
    if (!strcmp(s, "inc"))       return SCAN_INC;
    if (!strcmp(s, "dec"))       return SCAN_DEC;
    return -1;
}

/* ── JSON response builders ──────────────────────────────────────────────── */

static void json_ok(struct mg_connection *c) {
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
}

static void json_err(struct mg_connection *c, int code, const char *msg) {
    mg_http_reply(c, code, "Content-Type: application/json\r\n",
                  "{\"ok\":false,\"error\":\"%s\"}", msg);
}

/* ── async scan thread ───────────────────────────────────────────────────── */

typedef struct { uint64_t value; int width; int op; } scan_req_t;

static void *scan_thread_fn(void *arg) {
    scan_req_t *req = (scan_req_t *)arg;
    pthread_mutex_lock(&g_state.lock);
    scanner_first_scan(&g_state.scanner, req->value, req->width, req->op);
    g_state.scan_running = 0;
    g_state.scan_done    = 1;
    pthread_mutex_unlock(&g_state.lock);
    free(req);
    return NULL;
}

/* ── WebSocket helpers ───────────────────────────────────────────────────── */

static void build_watch_list_json(char *buf, size_t bufsz, int full) {
    int n = 0;
    if (full)
        n += snprintf(buf + n, bufsz - (size_t)n, "{\"type\":\"watch_list\",\"entries\":[");
    else
        n += snprintf(buf + n, bufsz - (size_t)n, "{\"type\":\"watch_update\",\"entries\":[");

    for (int i = 0; i < g_state.watchlist.count; i++) {
        watch_entry_t *e = &g_state.watchlist.entries[i];
        uint64_t val = 0;
        if (g_state.scanner.pid > 0)
            scanner_read(&g_state.scanner, e->addr, e->width, &val);

        if (full) {
            n += snprintf(buf + n, bufsz - (size_t)n,
                "%s{\"addr\":\"0x%llX\",\"label\":\"%s\",\"width\":%d,"
                "\"value\":%llu,\"frozen\":%s,\"freeze_value\":%llu}",
                i ? "," : "",
                (unsigned long long)e->addr, e->label, e->width,
                (unsigned long long)val,
                e->frozen ? "true" : "false",
                (unsigned long long)e->freeze_value);
        } else {
            n += snprintf(buf + n, bufsz - (size_t)n,
                "%s{\"addr\":\"0x%llX\",\"value\":%llu}",
                i ? "," : "",
                (unsigned long long)e->addr,
                (unsigned long long)val);
        }
    }
    snprintf(buf + n, bufsz - (size_t)n, "]}");
}

static void ws_broadcast(struct mg_mgr *mgr, const char *msg, size_t len) {
    for (struct mg_connection *c = mgr->conns; c; c = c->next)
        if (c->is_websocket)
            mg_ws_send(c, msg, len, WEBSOCKET_OP_TEXT);
}

void api_ws_broadcast_timer(void *arg) {
    struct mg_mgr *mgr = arg;

    /* trylock: if the scan thread holds the lock, skip this tick */
    if (pthread_mutex_trylock(&g_state.lock) != 0) return;

    int done    = g_state.scan_done;
    size_t cnt  = g_state.scanner.count;
    int capped  = g_state.scanner.capped;
    int permerr = g_state.scanner.perm_errors;
    if (done) g_state.scan_done = 0;

    char wbuf[8192];
    build_watch_list_json(wbuf, sizeof(wbuf), 0);
    pthread_mutex_unlock(&g_state.lock);

    if (done) {
        char sbuf[160];
        snprintf(sbuf, sizeof(sbuf),
            "{\"type\":\"scan_done\",\"candidate_count\":%zu%s%s}",
            cnt,
            capped  ? ",\"capped\":true"     : "",
            permerr ? ",\"perm_error\":true" : "");
        ws_broadcast(mgr, sbuf, strlen(sbuf));
    }
    ws_broadcast(mgr, wbuf, strlen(wbuf));
}

/* ── route handlers ──────────────────────────────────────────────────────── */

static void handle_status(struct mg_connection *c) {
    char pname[256] = "";
    pthread_mutex_lock(&g_state.lock);
    int pid            = (int)g_state.scanner.pid;
    const char *sstate = g_state.scanner.state == SCAN_STATE_IDLE
                         ? "idle" : "has_results";
    size_t ncand       = g_state.scanner.count;
    int nwatch         = g_state.watchlist.count;
    if (pid > 0) procutil_name((pid_t)pid, pname, sizeof(pname));
    pthread_mutex_unlock(&g_state.lock);

    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
        "{\"running\":true,\"attached_pid\":%d,\"process_name\":\"%s\","
        "\"scan_state\":\"%s\",\"candidate_count\":%zu,\"watch_count\":%d}",
        pid, pname, sstate, ncand, nwatch);
}

static void handle_processes(struct mg_connection *c) {
    proc_info_t procs[128];
    int n = procutil_list(procs, 128);

    char *buf = malloc((size_t)n * 64 + 32);
    if (!buf) { json_err(c, 500, "out of memory"); return; }

    int off = 0;
    off += sprintf(buf + off, "[");
    for (int i = 0; i < n; i++)
        off += sprintf(buf + off, "%s{\"pid\":%d,\"name\":\"%s\"}",
                       i ? "," : "", (int)procs[i].pid, procs[i].name);
    off += sprintf(buf + off, "]");

    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
    free(buf);
}

static void handle_attach(struct mg_connection *c, struct mg_http_message *hm) {
    double dpid = 0;
    if (!mg_json_get_num(hm->body, "$.pid", &dpid)) {
        json_err(c, 400, "missing pid"); return;
    }
    pid_t pid = (pid_t)(int)dpid;

    pthread_mutex_lock(&g_state.lock);
    int rc = scanner_attach(&g_state.scanner, pid);
    pthread_mutex_unlock(&g_state.lock);

    if (rc != 0) { json_err(c, 400, "process not found"); return; }
    json_ok(c);
}

static void handle_detach(struct mg_connection *c) {
    pthread_mutex_lock(&g_state.lock);
    scanner_detach(&g_state.scanner);
    pthread_mutex_unlock(&g_state.lock);
    json_ok(c);
}

static void handle_scan_start(struct mg_connection *c, struct mg_http_message *hm) {
    double dval = 0, dwidth = 4;
    char op_str[32] = "eq";
    mg_json_get_num(hm->body, "$.value", &dval);
    mg_json_get_num(hm->body, "$.width", &dwidth);
    json_get_str(hm->body, "$.op", op_str, sizeof(op_str));

    int op = op_from_str(op_str);
    if (op < 0 || op > SCAN_GT) { json_err(c, 400, "invalid op"); return; }

    int width = (int)dwidth;
    if (width != 1 && width != 2 && width != 4 && width != 8) {
        json_err(c, 400, "invalid width"); return;
    }

    pthread_mutex_lock(&g_state.lock);
    if (g_state.scanner.pid <= 0) {
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 400, "not attached"); return;
    }
    if (g_state.scan_running) {
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 409, "scan already running"); return;
    }

    scan_req_t *req = malloc(sizeof(scan_req_t));
    if (!req) {
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 500, "out of memory"); return;
    }
    req->value = (uint64_t)dval;
    req->width = width;
    req->op    = op;

    scanner_reset(&g_state.scanner);
    g_state.scan_running = 1;
    g_state.scan_done    = 0;
    pthread_mutex_unlock(&g_state.lock);

    pthread_t t;
    if (pthread_create(&t, NULL, scan_thread_fn, req) != 0) {
        free(req);
        pthread_mutex_lock(&g_state.lock);
        g_state.scan_running = 0;
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 500, "failed to start scan thread"); return;
    }
    pthread_detach(t);

    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                  "{\"ok\":true,\"scanning\":true}");
}

static void handle_scan_refine(struct mg_connection *c, struct mg_http_message *hm) {
    double dval = 0;
    char op_str[32] = "eq";
    mg_json_get_num(hm->body, "$.value", &dval);
    json_get_str(hm->body, "$.op", op_str, sizeof(op_str));

    int op = op_from_str(op_str);
    if (op < 0) { json_err(c, 400, "invalid op"); return; }

    pthread_mutex_lock(&g_state.lock);
    if (g_state.scanner.pid <= 0) {
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 400, "not attached"); return;
    }
    int rc     = scanner_refine(&g_state.scanner, (uint64_t)dval, op);
    size_t cnt = g_state.scanner.count;
    pthread_mutex_unlock(&g_state.lock);

    if (rc != 0) { json_err(c, 400, "no scan in progress"); return; }
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
        "{\"ok\":true,\"candidate_count\":%zu}", cnt);
}

static void handle_scan_reset(struct mg_connection *c) {
    pthread_mutex_lock(&g_state.lock);
    scanner_reset(&g_state.scanner);
    pthread_mutex_unlock(&g_state.lock);
    json_ok(c);
}

static void handle_scan_results(struct mg_connection *c, struct mg_http_message *hm) {
    size_t offset = 0, limit = 100;

    struct mg_str offval = mg_http_var(hm->query, mg_str("offset"));
    struct mg_str limval = mg_http_var(hm->query, mg_str("limit"));

    if (offval.buf && offval.len > 0) {
        char tmp[16] = "";
        size_t n = offval.len < 15 ? offval.len : 15;
        memcpy(tmp, offval.buf, n);
        offset = (size_t)atoi(tmp);
    }
    if (limval.buf && limval.len > 0) {
        char tmp[16] = "";
        size_t n = limval.len < 15 ? limval.len : 15;
        memcpy(tmp, limval.buf, n);
        limit = (size_t)atoi(tmp);
    }
    if (limit > 500) limit = 500;

    pthread_mutex_lock(&g_state.lock);
    size_t total = g_state.scanner.count;
    if (offset > total) offset = total;
    size_t avail = total - offset;
    size_t send  = avail < limit ? avail : limit;

    size_t bufsz = 64 + send * 48;
    char *buf = malloc(bufsz);
    if (!buf) {
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 500, "out of memory"); return;
    }

    int n = snprintf(buf, bufsz,
        "{\"total\":%zu,\"offset\":%zu,\"results\":[", total, offset);
    for (size_t i = 0; i < send; i++) {
        candidate_t *cd = &g_state.scanner.candidates[offset + i];
        n += snprintf(buf + n, bufsz - (size_t)n,
            "%s{\"addr\":\"0x%llX\",\"value\":%llu}",
            i ? "," : "",
            (unsigned long long)cd->addr,
            (unsigned long long)cd->last_value);
    }
    snprintf(buf + n, bufsz - (size_t)n, "]}");
    pthread_mutex_unlock(&g_state.lock);

    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
    free(buf);
}

static void handle_watch_get(struct mg_connection *c) {
    pthread_mutex_lock(&g_state.lock);
    size_t bufsz = 64 + (size_t)g_state.watchlist.count * 160;
    char *buf = malloc(bufsz);
    if (!buf) {
        pthread_mutex_unlock(&g_state.lock);
        json_err(c, 500, "out of memory"); return;
    }
    int n = sprintf(buf, "[");
    for (int i = 0; i < g_state.watchlist.count; i++) {
        watch_entry_t *e = &g_state.watchlist.entries[i];
        uint64_t val = 0;
        if (g_state.scanner.pid > 0)
            scanner_read(&g_state.scanner, e->addr, e->width, &val);
        n += snprintf(buf + n, bufsz - (size_t)n,
            "%s{\"addr\":\"0x%llX\",\"label\":\"%s\",\"width\":%d,"
            "\"value\":%llu,\"frozen\":%s,\"freeze_value\":%llu}",
            i ? "," : "",
            (unsigned long long)e->addr, e->label, e->width,
            (unsigned long long)val,
            e->frozen ? "true" : "false",
            (unsigned long long)e->freeze_value);
    }
    snprintf(buf + n, bufsz - (size_t)n, "]");
    pthread_mutex_unlock(&g_state.lock);

    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
    free(buf);
}

static void handle_watch_add(struct mg_connection *c, struct mg_http_message *hm) {
    char addr_str[32] = "", label[MAX_LABEL_LEN] = "";
    double dwidth = 4;
    json_get_str(hm->body, "$.addr",  addr_str, sizeof(addr_str));
    json_get_str(hm->body, "$.label", label,    sizeof(label));
    mg_json_get_num(hm->body, "$.width", &dwidth);

    if (!addr_str[0]) { json_err(c, 400, "missing addr"); return; }
    uint64_t addr = (uint64_t)strtoull(addr_str, NULL, 0);
    int width = (int)dwidth;
    if (width != 1 && width != 2 && width != 4 && width != 8) width = 4;

    pthread_mutex_lock(&g_state.lock);
    int rc = watch_add(&g_state.watchlist, addr, label, width);
    if (rc == 0) watch_save(&g_state.watchlist, g_state.watchfile);
    pthread_mutex_unlock(&g_state.lock);

    if (rc == -2) { json_err(c, 409, "already exists"); return; }
    if (rc != 0)  { json_err(c, 500, "watch list full"); return; }
    json_ok(c);
}

static void handle_watch_delete(struct mg_connection *c, uint64_t addr) {
    pthread_mutex_lock(&g_state.lock);
    int rc = watch_remove(&g_state.watchlist, addr);
    if (rc == 0) watch_save(&g_state.watchlist, g_state.watchfile);
    pthread_mutex_unlock(&g_state.lock);

    if (rc != 0) { json_err(c, 404, "not found"); return; }
    json_ok(c);
}

static void handle_watch_write(struct mg_connection *c,
                               struct mg_http_message *hm, uint64_t addr) {
    double dval = 0;
    if (!mg_json_get_num(hm->body, "$.value", &dval)) {
        json_err(c, 400, "missing value"); return;
    }

    pthread_mutex_lock(&g_state.lock);
    int idx = watch_find(&g_state.watchlist, addr);
    int rc  = -1;
    if (idx >= 0 && g_state.scanner.pid > 0)
        rc = scanner_write(&g_state.scanner, addr,
                           g_state.watchlist.entries[idx].width, (uint64_t)dval);
    pthread_mutex_unlock(&g_state.lock);

    if (idx < 0) { json_err(c, 404, "not found"); return; }
    if (rc != 0) { json_err(c, 500, "write failed"); return; }
    json_ok(c);
}

static void handle_watch_freeze(struct mg_connection *c,
                                struct mg_http_message *hm, uint64_t addr) {
    bool enabled = false;
    double dval  = 0;
    mg_json_get_bool(hm->body, "$.enabled", &enabled);
    mg_json_get_num(hm->body,  "$.value",   &dval);

    pthread_mutex_lock(&g_state.lock);
    int rc = watch_set_freeze(&g_state.watchlist, addr,
                              enabled ? 1 : 0, (uint64_t)dval);
    if (rc == 0) watch_save(&g_state.watchlist, g_state.watchfile);
    pthread_mutex_unlock(&g_state.lock);

    if (rc != 0) { json_err(c, 404, "not found"); return; }
    json_ok(c);
}

static void handle_watch_label(struct mg_connection *c,
                               struct mg_http_message *hm, uint64_t addr) {
    char label[MAX_LABEL_LEN] = "";
    json_get_str(hm->body, "$.label", label, sizeof(label));

    pthread_mutex_lock(&g_state.lock);
    int rc = watch_set_label(&g_state.watchlist, addr, label);
    if (rc == 0) watch_save(&g_state.watchlist, g_state.watchfile);
    pthread_mutex_unlock(&g_state.lock);

    if (rc != 0) { json_err(c, 404, "not found"); return; }
    json_ok(c);
}

static void handle_export_cht(struct mg_connection *c,
                              struct mg_http_message *hm) {
    char game_name[256] = "";
    json_get_str(hm->body, "$.game_name", game_name, sizeof(game_name));
    if (!game_name[0]) { json_err(c, 400, "missing game_name"); return; }

    pthread_mutex_lock(&g_state.lock);
    int rc = cht_export(g_state.cheatdir, game_name,
                        g_state.watchlist.entries, g_state.watchlist.count);
    pthread_mutex_unlock(&g_state.lock);

    if (rc != 0) { json_err(c, 500, "export failed"); return; }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.cht", g_state.cheatdir, game_name);
    mg_http_reply(c, 200, "Content-Type: application/json\r\n",
        "{\"ok\":true,\"path\":\"%s\"}", path);
}

static void send_watch_list_ws(struct mg_connection *c) {
    char buf[8192];
    pthread_mutex_lock(&g_state.lock);
    build_watch_list_json(buf, sizeof(buf), 1);
    pthread_mutex_unlock(&g_state.lock);
    mg_ws_send(c, buf, strlen(buf), WEBSOCKET_OP_TEXT);
}

/* ── main event handler ──────────────────────────────────────────────────── */

void api_handle(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_WS_OPEN) {
        send_watch_list_ws(c);
        return;
    }

    if (ev != MG_EV_HTTP_MSG) return;

    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    struct mg_str uri = hm->uri;

    if (uri_eq(uri, "/api/status") && is_method(hm, "GET")) {
        handle_status(c);
    } else if (uri_eq(uri, "/api/processes") && is_method(hm, "GET")) {
        handle_processes(c);
    } else if (uri_eq(uri, "/api/attach") && is_method(hm, "POST")) {
        handle_attach(c, hm);
    } else if (uri_eq(uri, "/api/detach") && is_method(hm, "POST")) {
        handle_detach(c);
    } else if (uri_eq(uri, "/api/scan/start") && is_method(hm, "POST")) {
        handle_scan_start(c, hm);
    } else if (uri_eq(uri, "/api/scan/refine") && is_method(hm, "POST")) {
        handle_scan_refine(c, hm);
    } else if (uri_eq(uri, "/api/scan/reset") && is_method(hm, "POST")) {
        handle_scan_reset(c);
    } else if (starts_with(uri, "/api/scan/results") && is_method(hm, "GET")) {
        handle_scan_results(c, hm);
    } else if (uri_eq(uri, "/api/watch") && is_method(hm, "GET")) {
        handle_watch_get(c);
    } else if (uri_eq(uri, "/api/watch") && is_method(hm, "POST")) {
        handle_watch_add(c, hm);
    } else if (starts_with(uri, "/api/watch/") && ends_with(uri, "/write")) {
        handle_watch_write(c, hm, extract_addr(uri));
    } else if (starts_with(uri, "/api/watch/") && ends_with(uri, "/freeze")) {
        handle_watch_freeze(c, hm, extract_addr(uri));
    } else if (starts_with(uri, "/api/watch/") && ends_with(uri, "/label")) {
        handle_watch_label(c, hm, extract_addr(uri));
    } else if (starts_with(uri, "/api/watch/") && is_method(hm, "DELETE")) {
        handle_watch_delete(c, extract_addr(uri));
    } else if (uri_eq(uri, "/api/export/cht") && is_method(hm, "POST")) {
        handle_export_cht(c, hm);
    } else if (uri_eq(uri, "/ws")) {
        mg_ws_upgrade(c, hm, NULL);
    } else if (starts_with(uri, "/api/")) {
        json_err(c, 404, "not found");
    } else {
        struct mg_http_serve_opts opts = {.root_dir = g_state.www_path};
        mg_http_serve_dir(c, hm, &opts);
    }
}
