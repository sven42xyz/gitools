/*
 * scan.c – recursive directory traversal to find git repositories
 */

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

#include "gitools.h"

static const char *SKIP_DIRS[] = { "vendor", "node_modules", ".git", NULL };

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
    snprintf(git_path, sizeof(git_path), "%s/.git", path);
    struct stat st;
    if (stat(git_path, &st) == 0) process_repo(path);

    DIR *dir = opendir(path);
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (should_skip(ent->d_name)) continue;

        char sub[PATH_MAX];
        snprintf(sub, sizeof(sub), "%s/%s", path, ent->d_name);

        struct stat sub_st;
        if (lstat(sub, &sub_st) != 0) continue;
        if (!S_ISDIR(sub_st.st_mode)) continue;
        if (S_ISLNK(sub_st.st_mode))  continue;  /* skip symlinks to avoid loops */

        find_repos(sub, depth + 1);
    }
    closedir(dir);
}
