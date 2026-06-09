#include "scanner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

/* aarch64 syscall numbers */
#ifndef __NR_process_vm_readv
#define __NR_process_vm_readv  270
#endif
#ifndef __NR_process_vm_writev
#define __NR_process_vm_writev 271
#endif

#define CANDIDATE_CHUNK  4096
#define READ_CHUNK_SIZE  (1 * 1024 * 1024)

static ssize_t vm_readv(pid_t pid, void *local, size_t llen,
                        void *remote, size_t rlen) {
    struct iovec l = { local, llen };
    struct iovec r = { remote, rlen };
    return syscall(__NR_process_vm_readv, (long)pid, &l, 1L, &r, 1L, 0L);
}

static ssize_t vm_writev(pid_t pid, const void *local, size_t llen,
                         void *remote, size_t rlen) {
    struct iovec l = { (void *)local, llen };
    struct iovec r = { remote, rlen };
    return syscall(__NR_process_vm_writev, (long)pid, &l, 1L, &r, 1L, 0L);
}

static uint64_t read_width(const uint8_t *p, int w) {
    switch (w) {
        case 1: return *p;
        case 2: { uint16_t v; memcpy(&v, p, 2); return v; }
        case 4: { uint32_t v; memcpy(&v, p, 4); return v; }
        case 8: { uint64_t v; memcpy(&v, p, 8); return v; }
        default: return 0;
    }
}

static void write_width(uint8_t *p, uint64_t val, int w) {
    switch (w) {
        case 1: *p = (uint8_t)val; break;
        case 2: { uint16_t v = (uint16_t)val; memcpy(p, &v, 2); break; }
        case 4: { uint32_t v = (uint32_t)val; memcpy(p, &v, 4); break; }
        case 8: memcpy(p, &val, 8); break;
    }
}

static int match_op(uint64_t val, uint64_t ref, uint64_t prev, int op) {
    switch (op) {
        case SCAN_EQ:        return val == ref;
        case SCAN_LT:        return val < ref;
        case SCAN_GT:        return val > ref;
        case SCAN_CHANGED:   return val != prev;
        case SCAN_UNCHANGED: return val == prev;
        case SCAN_INC:       return val > prev;
        case SCAN_DEC:       return val < prev;
        default:             return 0;
    }
}

static int add_candidate(scanner_t *s, uint64_t addr, uint64_t value) {
    if (s->count >= CANDIDATE_MAX) {
        s->capped = 1;
        return 0;
    }
    if (s->count >= s->capacity) {
        size_t new_cap = s->capacity + CANDIDATE_CHUNK;
        candidate_t *arr = realloc(s->candidates, new_cap * sizeof(candidate_t));
        if (!arr) return 0;
        s->candidates = arr;
        s->capacity = new_cap;
    }
    s->candidates[s->count].addr       = addr;
    s->candidates[s->count].last_value = value;
    s->count++;
    return 1;
}

void scanner_init(scanner_t *s) {
    memset(s, 0, sizeof(*s));
}

int scanner_attach(scanner_t *s, pid_t pid) {
    if (kill(pid, 0) != 0) return -1;
    scanner_reset(s);
    s->pid = pid;
    return 0;
}

void scanner_detach(scanner_t *s) {
    scanner_reset(s);
    s->pid = 0;
}

int scanner_first_scan(scanner_t *s, uint64_t value, int width, int op) {
    if (s->pid <= 0) return -1;
    if (width != 1 && width != 2 && width != 4 && width != 8) return -1;

    scanner_reset(s);
    s->width = width;

    s->perm_errors = 0;

    char maps_path[64];
    snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", (int)s->pid);
    FILE *f = fopen(maps_path, "r");
    if (!f) return -1;

    uint8_t *buf = malloc(READ_CHUNK_SIZE);
    if (!buf) { fclose(f); return -1; }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        uint64_t start, end;
        char perms[8] = "";
        if (sscanf(line, "%llx-%llx %7s",
                   (unsigned long long *)&start,
                   (unsigned long long *)&end,
                   perms) != 3)
            continue;

        /* readable + writable, non-special regions */
        if (perms[0] != 'r' || perms[1] != 'w') continue;
        if (end <= start) continue;

        size_t region_size = (size_t)(end - start);
        size_t offset = 0;

        while (offset < region_size) {
            size_t chunk = region_size - offset;
            if (chunk > READ_CHUNK_SIZE) chunk = READ_CHUNK_SIZE;
            chunk = (chunk / (size_t)width) * (size_t)width;
            if (chunk == 0) break;

            ssize_t n = vm_readv(s->pid, buf, chunk,
                                 (void *)(uintptr_t)(start + offset), chunk);
            if (n <= 0) {
                if (errno == EPERM) s->perm_errors++;
                offset += chunk;
                continue;
            }

            for (size_t i = 0; (i + (size_t)width) <= (size_t)n; i += (size_t)width) {
                uint64_t val = read_width(buf + i, width);
                if (match_op(val, value, 0, op)) {
                    if (!add_candidate(s, start + offset + i, val))
                        goto done;
                }
            }
            offset += chunk;
        }
    }

done:
    free(buf);
    fclose(f);
    s->state = SCAN_STATE_HAS_RESULTS;
    return 0;
}

int scanner_refine(scanner_t *s, uint64_t value, int op) {
    if (s->pid <= 0 || s->state == SCAN_STATE_IDLE) return -1;

    size_t new_count = 0;
    for (size_t i = 0; i < s->count; i++) {
        uint64_t cur = 0;
        if (scanner_read(s, s->candidates[i].addr, s->width, &cur) != 0)
            continue;
        if (match_op(cur, value, s->candidates[i].last_value, op)) {
            s->candidates[new_count].addr       = s->candidates[i].addr;
            s->candidates[new_count].last_value = cur;
            new_count++;
        }
    }
    s->count  = new_count;
    s->capped = 0;
    return 0;
}

void scanner_reset(scanner_t *s) {
    free(s->candidates);
    s->candidates = NULL;
    s->count      = 0;
    s->capacity   = 0;
    s->capped     = 0;
    s->perm_errors = 0;
    s->state      = SCAN_STATE_IDLE;
}

int scanner_read(scanner_t *s, uint64_t addr, int width, uint64_t *out) {
    if (s->pid <= 0) return -1;
    uint8_t buf[8] = {0};
    ssize_t n = vm_readv(s->pid, buf, (size_t)width,
                         (void *)(uintptr_t)addr, (size_t)width);
    if (n != width) return -1;
    *out = read_width(buf, width);
    return 0;
}

int scanner_write(scanner_t *s, uint64_t addr, int width, uint64_t value) {
    if (s->pid <= 0) return -1;
    uint8_t buf[8] = {0};
    write_width(buf, value, width);
    ssize_t n = vm_writev(s->pid, buf, (size_t)width,
                          (void *)(uintptr_t)addr, (size_t)width);
    if (n != width) {
        if (errno == EPERM)
            fprintf(stderr, "scanner_write: EPERM at 0x%llx — insufficient privileges\n",
                    (unsigned long long)addr);
        return -1;
    }
    return 0;
}
