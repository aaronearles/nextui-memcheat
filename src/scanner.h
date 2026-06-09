#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define SCAN_EQ        0
#define SCAN_LT        1
#define SCAN_GT        2
#define SCAN_CHANGED   3
#define SCAN_UNCHANGED 4
#define SCAN_INC       5
#define SCAN_DEC       6

#define CANDIDATE_MAX  200000

typedef enum {
    SCAN_STATE_IDLE,
    SCAN_STATE_HAS_RESULTS
} scan_state_t;

typedef struct {
    uint64_t addr;
    uint64_t last_value;
} candidate_t;

typedef struct {
    pid_t       pid;
    candidate_t *candidates;
    size_t      count;
    size_t      capacity;
    int         width;
    scan_state_t state;
    int         capped;
    int         perm_errors; /* vm_readv EPERM count from last scan */
} scanner_t;

void scanner_init(scanner_t *s);
int  scanner_attach(scanner_t *s, pid_t pid);
void scanner_detach(scanner_t *s);
int  scanner_first_scan(scanner_t *s, uint64_t value, int width, int op);
int  scanner_refine(scanner_t *s, uint64_t value, int op);
void scanner_reset(scanner_t *s);
int  scanner_read(scanner_t *s, uint64_t addr, int width, uint64_t *out);
int  scanner_write(scanner_t *s, uint64_t addr, int width, uint64_t value);
