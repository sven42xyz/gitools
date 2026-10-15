/*
 * config.c – load ~/.gitlsrc configuration file
 *
 * Format:
 *   # comment
 *   default_dir=~/projects
 *   max_depth=3
 *   skip_dirs=build,dist,tmp
 *   protected_branches=develop,staging
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

#define MAX_LIST_ITEMS 64

/*
 * Split a comma-separated value into a newly allocated list (capped to avoid
 * memory exhaustion), trimming surrounding blanks so "a, b" stores "b" rather
 * than " b". On success the previous list is freed and
 * replaced; on allocation failure it is left untouched.
 */
static void set_list(const char *val, char ***arr, size_t *count) {
    char *copy = strdup(val);
    if (!copy) return;

    size_t cap = 1;
    for (const char *p = copy; *p; p++)
        if (*p == ',') cap++;
    if (cap > MAX_LIST_ITEMS) cap = MAX_LIST_ITEMS;

    char **list = malloc(cap * sizeof(char *));
    if (!list) { free(copy); return; }

    size_t n = 0;
    for (char *tok = strtok(copy, ","); tok && n < cap; tok = strtok(NULL, ",")) {
        while (*tok == ' ' || *tok == '\t') tok++;
        char *end = tok + strlen(tok);
        while (end > tok && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        if (*tok == '\0') continue;

        char *dup = strdup(tok);
        if (!dup) {
            for (size_t i = 0; i < n; i++) free(list[i]);
            free(list);
            free(copy);
            return;
        }
        list[n++] = dup;
    }
    free(copy);

    for (size_t i = 0; i < *count; i++) free((*arr)[i]);
    free(*arr);
    *arr   = list;
    *count = n;
}

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
            set_list(val, &opt_extra_skip, &opt_extra_skip_count);

        } else if (strcmp(key, "protected_branches") == 0) {
            set_list(val, &opt_protected_branches, &opt_protected_branches_count);

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
    }

    fclose(f);
}
