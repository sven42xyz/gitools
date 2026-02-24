/*
 * scan.c – recursive directory traversal to find git repositories
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#include "gitools.h"

static const char * const SKIP_DIRS[] = { "vendor", "node_modules", ".git", NULL };

static int should_skip(const char *name) {
    for (int i = 0; SKIP_DIRS[i]; i++)
        if (strcmp(name, SKIP_DIRS[i]) == 0) return 1;
    if (!opt_all && name[0] == '.') return 1;
    return 0;
}

void find_repos(const char *path, int depth) {
    if (depth > opt_max_depth) return;

    /* if this directory is a git repo, process it then keep recursing */
    char git_path[PATH_MAX];
    int n = snprintf(git_path, sizeof(git_path), "%s/.git", path);
    if (n > 0 && n < (int)sizeof(git_path)) {
        struct stat st;
        if (stat(git_path, &st) == 0) process_repo(path);
    }

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (should_skip(ent->d_name)) continue;

        char sub[PATH_MAX];
        int m = snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);
        if (m <= 0 || m >= (int)sizeof(sub)) continue;  /* path too long, skip */

        struct stat sub_st;
        /* lstat: S_ISDIR is false for symlinks, so symlinks are already skipped */
        if (lstat(sub, &sub_st) != 0) continue;
        if (!S_ISDIR(sub_st.st_mode)) continue;

        find_repos(sub, depth + 1);
    }
    closedir(dir);
}
