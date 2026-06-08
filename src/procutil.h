#pragma once
#include <sys/types.h>
#include <stddef.h>

#define MAX_PROC_NAME 256

typedef struct {
    pid_t pid;
    char  name[MAX_PROC_NAME];
} proc_info_t;

int procutil_list(proc_info_t *results, int max);
int procutil_name(pid_t pid, char *buf, size_t len);
int procutil_exists(pid_t pid);
