/*
 * config.c – load ~/.gitlsrc configuration file
 *
 * Format:
 *   # comment
 *   default_dir=~/projects
 *   max_depth=3
 *   skip_dirs=build,dist,tmp
 *   no_color=true
 *
 * Set GITLS_CONFIG=/path/to/file to override the default ~/.gitlsrc path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <pwd.h>
#include <unistd.h>

#include "gitools.h"

void load_config(void) {
    char path[PATH_MAX];

    /* Allow override via environment variable (useful for testing) */
    const char *env = getenv("GITLS_CONFIG");
    if (env) {
        strncpy(path, env, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        struct passwd *pw = getpwuid(getuid());
        if (!pw) return;
        int n = snprintf(path, sizeof(path), "%s/.gitlsrc", pw->pw_dir);
        if (n <= 0 || n >= (int)sizeof(path)) return;
    }

    FILE *f = fopen(path, "r");
    if (!f) return;   /* no config file is fine */

    struct passwd *pw = getpwuid(getuid());  /* for ~ expansion */

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        /* strip trailing newline / carriage return */
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';

        if (len == 0 || line[0] == '#') continue;  /* blank / comment */

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = line;
        const char *val = eq + 1;

        if (strcmp(key, "default_dir") == 0) {
            if (val[0] == '~' && val[1] == '/' && pw) {
                snprintf(opt_default_dir, sizeof(opt_default_dir),
                         "%s/%s", pw->pw_dir, val + 2);
            } else if (strcmp(val, "~") == 0 && pw) {
                snprintf(opt_default_dir, sizeof(opt_default_dir), "%s", pw->pw_dir);
            } else {
                snprintf(opt_default_dir, sizeof(opt_default_dir), "%s", val);
            }

        } else if (strcmp(key, "max_depth") == 0) {
            char *end;
            errno = 0;
            long d = strtol(val, &end, 10);
            if (*end == '\0' && errno != ERANGE && d >= 0 && d <= INT_MAX)
                opt_max_depth = (int)d;

        } else if (strcmp(key, "skip_dirs") == 0) {
#define MAX_SKIP_DIRS 64
            /* split comma-separated list (capped to avoid memory exhaustion) */
            char *copy = strdup(val);
            if (!copy) continue;

            size_t count = 1;
            for (const char *p = copy; *p; p++)
                if (*p == ',') count++;
            if (count > MAX_SKIP_DIRS) count = MAX_SKIP_DIRS;

            char **arr = malloc(count * sizeof(char *));
            if (!arr) { free(copy); continue; }

            size_t idx = 0;
            char *tok = strtok(copy, ",");
            while (tok && idx < count) {
                char *dup = strdup(tok);
                if (!dup) {
                    for (size_t i = 0; i < idx; i++) free(arr[i]);
                    free(arr);
                    free(copy);
                    goto next_line;
                }
                arr[idx++] = dup;
                tok = strtok(NULL, ",");
            }

            /* free any previous list */
            if (opt_extra_skip) {
                for (size_t i = 0; i < opt_extra_skip_count; i++)
                    free(opt_extra_skip[i]);
                free(opt_extra_skip);
            }
            opt_extra_skip       = arr;
            opt_extra_skip_count = idx;
            free(copy);

        } else if (strcmp(key, "watch_interval") == 0) {
            char *end;
            errno = 0;
            long iv = strtol(val, &end, 10);
            if (*end == '\0' && errno != ERANGE && iv >= 1 && iv <= INT_MAX)
                opt_watch_interval = (int)iv;

        } else if (strcmp(key, "dirty_only") == 0) {
            if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0)
                opt_dirty_only = true;

        } else if (strcmp(key, "no_color") == 0) {
            if (strcmp(val, "true") == 0 || strcmp(val, "1") == 0)
                opt_no_color = true;

        } else if (strcmp(key, "categories") == 0) {
            /* default is on, so only an explicit false/0 turns it off */
            if (strcmp(val, "false") == 0 || strcmp(val, "0") == 0)
                opt_categories = false;
        }
        next_line: ;   /* jump target for strdup failure in skip_dirs */
    }

    fclose(f);
}
