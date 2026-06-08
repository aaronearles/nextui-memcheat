#include "cht.h"
#include <stdio.h>
#include <string.h>

int cht_export(const char *cheatdir, const char *game_name,
               const watch_entry_t *entries, int count) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.cht", cheatdir, game_name);

    FILE *f = fopen(path, "w");
    if (!f) return -1;

    fprintf(f, "cheats = %d\n\n", count);

    for (int i = 0; i < count; i++) {
        fprintf(f, "cheat%d_desc = \"%s\"\n", i, entries[i].label);
        fprintf(f, "cheat%d_code = \"%08llX+%08llX\"\n", i,
                (unsigned long long)entries[i].addr,
                (unsigned long long)entries[i].freeze_value);
        fprintf(f, "cheat%d_enable = false\n", i);
        if (i + 1 < count) fprintf(f, "\n");
    }

    fclose(f);
    return 0;
}
