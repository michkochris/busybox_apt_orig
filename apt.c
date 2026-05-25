/* vi: set sw=4 ts=4: */
/*
 * busybox_apt: A lightweight implementation of APT for BusyBox.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */

//config:config APT
//config:   bool "apt (8 kb)"
//config:   default y
//config:   select DPKG
//config:   help
//config:   apt is a high-level package management frontend. It handles
//config:   repository management and dependency resolution as a
//config:   frontend to dpkg. It includes unique rescue features to
//config:   manually extract packages using built-in utilities if the
//config:   system package manager is broken.

//applet:IF_APT(APPLET(apt, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_APT) += apt.o

//usage:#define apt_trivial_usage
//usage:       "[-f] COMMAND [PACKAGE...]"
//usage:#define apt_full_usage "\n\n"
//usage:       "High-level package management frontend\n"
//usage:     "\nOptions:"
//usage:     "\n    -f, --fix-broken   Pass --force-depends to dpkg"
//usage:     "\nCommands:"
//usage:     "\n    update    Update list of available packages"
//usage:     "\n    install       Install new packages"
//usage:     "\n    remove    Remove packages"
//usage:     "\n    upgrade       Upgrade the system"
//usage:     "\n    reinstall  Reinstall packages (restores files)"
//usage:     "\n    rescue-install Install packages bypassing dpkg (uses internal ar/tar to /)"
//usage:     "\n    verify    Verify package sanity (deps, files & symlinks)"
//usage:     "\n    md5check   Verify package integrity (checks md5sums)"
//usage:     "\n    list --upgradable  Show packages with available updates"
//usage:     "\n    search    Search for a package"

#include "libbb.h"
#include "bb_archive.h"
#include "common_bufsiz.h"
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/statvfs.h>
#include <ctype.h>

/* ANSI Colors */
#define CLR_GREEN  "\033[0;32m"
#define CLR_RED    "\033[0;31m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"

#define MAX_DEP_DEPTH 50

typedef struct repo_s {
    char *uri;
    char *dist;
    llist_t *components;
    struct repo_s *next;
} repo_t;

typedef struct pkg_s {
    char *name;
    char *version;
    char *depends;
    char *pre_depends;
    char *recommends;
    char *provides;
    char *description;
    char *filename;
    char *repo_uri;
    long size;
    long installed_size;
    int state; /* 0: unvisited, 1: queued, 2: installing */
    struct pkg_s *next;
} pkg_t;

struct globals {
    pkg_t *all_packages;
    pkg_t **pkg_array;  /* For O(log n) lookups */
    int pkg_count;
    llist_t *install_queue;
    llist_t *installed_packages;
    char bb_path[1024];
} FIX_ALIASING;
#define G (*(struct globals*)bb_common_bufsiz1)

static void get_bb_exe(char *buf, size_t size)
{
    ssize_t len = readlink("/proc/self/exe", buf, size - 1);
    if (len == -1) strcpy(buf, "busybox");
    else buf[len] = '\0';
}

/* Safe execution helper avoiding `/bin/sh` injection */
static int run_cmd(char *const argv[])
{
    pid_t pid;
    int status;

    pid = vfork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child */
        /* Redirect stderr to /dev/null for quietness if desired, though omitted here for standard BB behavior */
        execvp(argv[0], argv);
        _exit(127); /* If exec fails */
    }
    
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

static int download_file(const char *url, const char *dest)
{
    int rc;
    char *argv_sys[] = {(char*)"wget", (char*)"-q", (char*)"-O", (char *)dest, (char *)url, NULL};
    char *argv_bb[] = {G.bb_path, (char*)"wget", (char*)"-q", (char*)"-O", (char *)dest, (char *)url, NULL};

    /* 1. Try system wget */
    rc = run_cmd(argv_sys);
    if (rc == 0) return 0;

    /* 2. Fallback using our exact binary path */
    rc = run_cmd(argv_bb);
    if (rc != 0) {
       bb_error_msg("Both system and internal wget failed to download %s", url);
    }
    return rc;
}

static int dpkg_call(char **argv_ext)
{
    int rc, count, i;
    char **bb_argv;
    
    /* Modify argv_ext[0] inline to point to correct binary */
    argv_ext[0] = (char*)"dpkg";
    rc = run_cmd(argv_ext);
    if (rc == 0) return 0;

    /* Fallback to internal dpkg */
    /* Shift array right requires realloc in caller, so we build a new one */
    count = 0;
    while(argv_ext[count]) count++;
    
    bb_argv = xmalloc(sizeof(char*) * (count + 2));
    bb_argv[0] = G.bb_path;
    for(i = 0; i <= count; i++) {
        bb_argv[i+1] = argv_ext[i];
    }
    bb_argv[1] = (char*)"dpkg"; /* Overwrite the shifted "dpkg" with just the command name */

    rc = run_cmd(bb_argv);
    free(bb_argv);
    return rc;
}

static int md5_check(const char *md5, const char *path)
{
    int rc, fd;
    char tmp_file[] = "/tmp/md5.XXXXXX";
    char *argv_sys[6];
    char *argv_bb[7];

    fd = mkstemp(tmp_file);
    if (fd < 0) return -1;

    dprintf(fd, "%s  %s\n", md5, path);
    close(fd);

    argv_sys[0] = (char*)"md5sum";
    argv_sys[1] = (char*)"-c";
    argv_sys[2] = (char*)"--status";
    argv_sys[3] = tmp_file;
    argv_sys[4] = NULL;

    argv_bb[0] = G.bb_path;
    argv_bb[1] = (char*)"md5sum";
    argv_bb[2] = (char*)"-c";
    argv_bb[3] = (char*)"--status";
    argv_bb[4] = tmp_file;
    argv_bb[5] = NULL;

    rc = run_cmd(argv_sys);
    if (rc != 0) rc = run_cmd(argv_bb);

    unlink(tmp_file);
    return rc;
}

static void update_progress(int current, int total, const char *msg UNUSED_PARAM)
{
    unsigned width, height;
    int i, pos, percent;
    int bar_space;

    get_terminal_width_height(STDOUT_FILENO, &width, &height);
    if (width < 30) return;

    percent = (current * 100) / total;
    bar_space = width - 24;
    if (bar_space < 10) bar_space = 10;
    pos = (percent * bar_space) / 100;

    printf("\033[%d;1H\033[K", height);
    printf("Progress: [%3d%%] [", percent);
    for (i = 0; i < bar_space; i++) {
       if (i < pos) printf("#");
       else printf(".");
    }
    printf("]");
    fflush(stdout);
}

static int llist_count(llist_t *list)
{
    int count = 0;
    while (list) {
       count++;
       list = list->link;
    }
    return count;
}

static repo_t *parse_sources_list(const char *filename)
{
    FILE *f = fopen_for_read(filename);
    repo_t *repos = NULL;
    char *line;

    if (!f) return NULL;

    while ((line = xmalloc_fgetline(f)) != NULL) {
       char *ptr = skip_whitespace(line);
       char *type, *uri, *dist;

       if (*ptr == '#' || *ptr == '\0') {
          free(line);
          continue;
       }

       type = strtok(ptr, " \t");
       if (!type || strcmp(type, "deb") != 0) {
          free(line);
          continue;
       }

       uri = strtok(NULL, " \t");
       dist = strtok(NULL, " \t");

       if (uri && dist) {
          repo_t *new_repo = xzalloc(sizeof(repo_t));
          char *comp;

          new_repo->uri = xstrdup(uri);
          new_repo->dist = xstrdup(dist);

          while ((comp = strtok(NULL, " \t")) != NULL) {
             llist_add_to(&new_repo->components, xstrdup(comp));
          }

          new_repo->next = repos;
          repos = new_repo;
       }
       free(line);
    }
    fclose(f);
    return repos;
}

static char *uri_to_filename(const char *uri)
{
    char *res = xstrdup(uri);
    char *p = res;
    char *s, *ret;

    if (strncmp(p, "http://", 7) == 0) p += 7;
    else if (strncmp(p, "https://", 8) == 0) p += 8;

    s = p;
    while (*s) {
       if (*s == '/' || *s == ':') *s = '_';
       s++;
    }
    ret = xstrdup(p);
    free(res);
    return ret;
}

static const char *get_debian_arch(void)
{
    struct utsname uts;
    if (uname(&uts) < 0) return "amd64";
    if (strcmp(uts.machine, "x86_64") == 0) return "amd64";
    if (strcmp(uts.machine, "aarch64") == 0) return "arm64";
    if (strncmp(uts.machine, "arm", 3) == 0) return "armhf";
    if (strstr(uts.machine, "86")) return "i386";
    return "amd64";
}

static void update_repos(repo_t *repos)
{
    repo_t *curr = repos;
    const char *arch = get_debian_arch();
    int count = 1;

    mkdir("/var/lib/apt", 0755);
    mkdir("/var/lib/apt/lists", 0755);

    while (curr) {
       llist_t *comp = curr->components;
       char *uri_part = uri_to_filename(curr->uri);

       while (comp) {
          char *url = xasprintf("%s/dists/%s/%s/binary-%s/Packages.gz",
             curr->uri, curr->dist, (char *)comp->data, arch);
          char *local_name = xasprintf("%s_dists_%s_%s_binary-%s_Packages.gz",
             uri_part, curr->dist, (char *)comp->data, arch);
          char *local_path = xasprintf("/var/lib/apt/lists/%s", local_name);

          printf("%sGet:%d%s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, url, (char *)comp->data);

          download_file(url, local_path);

          free(local_path);
          free(local_name);
          free(url);
          comp = comp->link;
       }
       free(uri_part);
       curr = curr->next;
    }
}

static void add_package(pkg_t *pkg)
{
    pkg->next = G.all_packages;
    G.all_packages = pkg;
}

static int cmp_pkg(const void *a, const void *b)
{
    pkg_t *pa = *(pkg_t **)a;
    pkg_t *pb = *(pkg_t **)b;
    return strcmp(pa->name, pb->name);
}

static void finalize_packages(void)
{
    pkg_t *p = G.all_packages;
    int count = 0, i;
    
    while (p) { count++; p = p->next; }
    G.pkg_count = count;
    G.pkg_array = xmalloc(count * sizeof(pkg_t *));
    
    p = G.all_packages;
    for (i = 0; i < count; i++) {
        G.pkg_array[i] = p;
        p = p->next;
    }
    
    qsort(G.pkg_array, count, sizeof(pkg_t *), cmp_pkg);
}

static pkg_t *find_package(const char *name)
{
    int l = 0, r = G.pkg_count - 1, i;

    /* O(log n) Binary Search for Exact Match */
    while (l <= r) {
        int m = l + (r - l) / 2;
        int cmp = strcmp(G.pkg_array[m]->name, name);
        if (cmp == 0) return G.pkg_array[m];
        if (cmp < 0) l = m + 1;
        else r = m - 1;
    }

    /* O(n) Fallback Search for 'Provides' virtual packages */
    for (i = 0; i < G.pkg_count; i++) {
        pkg_t *p = G.pkg_array[i];
        if (p->provides) {
            char *prov_copy = xstrdup(p->provides);
            char *saveptr;
            char *token = strtok_r(prov_copy, ",", &saveptr);
            while (token) {
                char *clean = skip_whitespace(token);
                char *p_spec = strpbrk(clean, " (:");
                if (p_spec) *p_spec = '\0';
                if (strcmp(clean, name) == 0) {
                    free(prov_copy);
                    return p;
                }
                token = strtok_r(NULL, ",", &saveptr);
            }
            free(prov_copy);
        }
    }
    return NULL;
}

typedef struct {
    char *name;
    long size;
} pkg_info_t;

static bool is_installed(const char *pkg_name)
{
    llist_t *curr = G.installed_packages;
    while (curr) {
       pkg_info_t *info = (pkg_info_t *)curr->data;
       if (strcmp(info->name, pkg_name) == 0) return true;
       curr = curr->link;
    }
    return false;
}

static long get_installed_size(const char *pkg_name)
{
    llist_t *curr = G.installed_packages;
    while (curr) {
       pkg_info_t *info = (pkg_info_t *)curr->data;
       if (strcmp(info->name, pkg_name) == 0) return info->size;
       curr = curr->link;
    }
    return 0;
}

static void load_installed_packages(void)
{
    FILE *f = fopen_for_read("/var/lib/dpkg/status");
    char *line;
    char *curr_pkg = NULL;
    long curr_size = 0;
    bool installed = false;

    if (!f) return;

    while ((line = xmalloc_fgetline(f)) != NULL) {
       if (strncmp(line, "Package: ", 9) == 0) {
          curr_pkg = xstrdup(skip_whitespace(line + 9));
       } else if (strncmp(line, "Status: ", 8) == 0) {
          if (strstr(line, "installed")) installed = true;
       } else if (strncmp(line, "Installed-Size: ", 16) == 0) {
          curr_size = atol(skip_whitespace(line + 16));
       } else if (strncmp(line, "Provides: ", 10) == 0) {
          if (installed && curr_pkg) {
             char *prov_copy = xstrdup(skip_whitespace(line + 10));
             char *saveptr;
             char *token = strtok_r(prov_copy, ",", &saveptr);
             while (token) {
                char *clean = skip_whitespace(token);
                char *p_spec = strpbrk(clean, " (:");
                pkg_info_t *info;
                if (p_spec) *p_spec = '\0';

                info = xzalloc(sizeof(pkg_info_t));
                info->name = xstrdup(clean);
                info->size = 0;
                llist_add_to(&G.installed_packages, info);

                token = strtok_r(NULL, ",", &saveptr);
             }
             free(prov_copy);
          }
       } else if (*line == '\0') {
          if (installed && curr_pkg) {
             pkg_info_t *info = xzalloc(sizeof(pkg_info_t));
             info->name = curr_pkg;
             info->size = curr_size;
             llist_add_to(&G.installed_packages, info);
             curr_pkg = NULL;
          }
          free(curr_pkg); curr_pkg = NULL;
          installed = false;
          curr_size = 0;
       }
       free(line);
    }
    /* Edge case: EOF without trailing newline */
    if (installed && curr_pkg) {
        pkg_info_t *info = xzalloc(sizeof(pkg_info_t));
        info->name = curr_pkg;
        info->size = curr_size;
        llist_add_to(&G.installed_packages, info);
    } else {
        free(curr_pkg);
    }
    fclose(f);
}

static void resolve_deps(pkg_t *p, bool force, int depth)
{
    char *deps_copy, *saveptr, *dep_entry;

    if (!p || p->state != 0) return;
    if (depth > MAX_DEP_DEPTH) {
        bb_error_msg("Dependency resolution depth limit exceeded for %s", p->name);
        return;
    }
    if (!force && is_installed(p->name)) return;

    p->state = 1; /* Mark as queued */

    /* Handle Pre-Depends first */
    if (p->pre_depends) {
       char *pre_copy = xstrdup(p->pre_depends);
       char *pre_saveptr;
       char *pre_entry = strtok_r(pre_copy, ",", &pre_saveptr);
       while (pre_entry) {
          char *or_saveptr, *alt;
          char *dep_entry_copy = xstrdup(pre_entry);
          pkg_t *to_install = NULL;
          bool satisfied = false;

          alt = strtok_r(dep_entry_copy, "|", &or_saveptr);
          while (alt) {
             char *clean_alt = skip_whitespace(alt);
             char *p_spec = strpbrk(clean_alt, " (:");
             if (p_spec) *p_spec = '\0';
             if (is_installed(clean_alt)) { satisfied = true; break; }
             alt = strtok_r(NULL, "|", &or_saveptr);
          }
          free(dep_entry_copy);

          if (!satisfied) {
             dep_entry_copy = xstrdup(pre_entry);
             alt = strtok_r(dep_entry_copy, "|", &or_saveptr);
             while (alt) {
                char *clean_alt = skip_whitespace(alt);
                char *p_spec = strpbrk(clean_alt, " (:");
                if (p_spec) *p_spec = '\0';
                to_install = find_package(clean_alt);
                if (to_install) break;
                alt = strtok_r(NULL, "|", &or_saveptr);
             }
             free(dep_entry_copy);
             if (to_install) resolve_deps(to_install, false, depth + 1);
          }
          pre_entry = strtok_r(NULL, ",", &pre_saveptr);
       }
       free(pre_copy);
    }

    if (p->depends) {
       deps_copy = xstrdup(p->depends);
       dep_entry = strtok_r(deps_copy, ",", &saveptr);

       while (dep_entry) {
          char *or_saveptr, *alt, *dep_entry_copy;
          pkg_t *to_install = NULL;
          bool satisfied = false;

          dep_entry_copy = xstrdup(dep_entry);
          alt = strtok_r(dep_entry_copy, "|", &or_saveptr);
          while (alt) {
             char *clean_alt = skip_whitespace(alt);
             char *p_spec = strpbrk(clean_alt, " (:");
             if (p_spec) *p_spec = '\0';

             if (is_installed(clean_alt)) {
                satisfied = true; break;
             }
             alt = strtok_r(NULL, "|", &or_saveptr);
          }
          free(dep_entry_copy);

          if (!satisfied) {
             dep_entry_copy = xstrdup(dep_entry);
             alt = strtok_r(dep_entry_copy, "|", &or_saveptr);
             while (alt) {
                char *clean_alt = skip_whitespace(alt);
                char *p_spec = strpbrk(clean_alt, " (:");
                if (p_spec) *p_spec = '\0';

                to_install = find_package(clean_alt);
                if (to_install) break;
                alt = strtok_r(NULL, "|", &or_saveptr);
             }
             free(dep_entry_copy);
             if (to_install) resolve_deps(to_install, false, depth + 1);
          }
          dep_entry = strtok_r(NULL, ",", &saveptr);
       }
       free(deps_copy);
    }

    llist_add_to_end(&G.install_queue, p);
}

static void parse_package_file(const char *filename, const char *repo_uri)
{
    int fd = open_zipped(filename, 0);
    FILE *f;
    char *line;
    pkg_t *curr = NULL;
    char **last_field = NULL;

    if (fd < 0) return;
    f = fdopen(fd, "r");

    while ((line = xmalloc_fgetline(f)) != NULL) {
       if (*line == '\0') {
          if (curr) { add_package(curr); curr = NULL; }
          last_field = NULL;
       } else if (*line == ' ' || *line == '\t') {
          if (curr && last_field && *last_field) {
             char *combined = xasprintf("%s %s", *last_field, skip_whitespace(line));
             free(*last_field);
             *last_field = combined;
          }
       } else if (strncmp(line, "Package: ", 9) == 0) {
          if (curr) add_package(curr);
          curr = xzalloc(sizeof(pkg_t));
          curr->name = xstrdup(skip_whitespace(line + 9));
          curr->repo_uri = xstrdup(repo_uri);
          last_field = &curr->name;
       } else if (curr && strncmp(line, "Version: ", 9) == 0) {
          curr->version = xstrdup(skip_whitespace(line + 9));
          last_field = &curr->version;
       } else if (curr && strncmp(line, "Depends: ", 9) == 0) {
          curr->depends = xstrdup(skip_whitespace(line + 9));
          last_field = &curr->depends;
       } else if (curr && strncmp(line, "Pre-Depends: ", 13) == 0) {
          curr->pre_depends = xstrdup(skip_whitespace(line + 13));
          last_field = &curr->pre_depends;
       } else if (curr && strncmp(line, "Recommends: ", 12) == 0) {
          curr->recommends = xstrdup(skip_whitespace(line + 12));
          last_field = &curr->recommends;
       } else if (curr && strncmp(line, "Provides: ", 10) == 0) {
          curr->provides = xstrdup(skip_whitespace(line + 10));
          last_field = &curr->provides;
       } else if (curr && strncmp(line, "Description: ", 13) == 0) {
          curr->description = xstrdup(skip_whitespace(line + 13));
          last_field = &curr->description;
       } else if (curr && strncmp(line, "Filename: ", 10) == 0) {
          curr->filename = xstrdup(skip_whitespace(line + 10));
          last_field = &curr->filename;
       } else if (curr && strncmp(line, "Size: ", 6) == 0) {
          curr->size = atol(skip_whitespace(line + 6));
          last_field = NULL;
       } else if (curr && strncmp(line, "Installed-Size: ", 16) == 0) {
          curr->installed_size = atol(skip_whitespace(line + 16));
          last_field = NULL;
       } else {
          last_field = NULL;
       }
       free(line);
    }
    if (curr) add_package(curr);
    fclose(f);
}

static void load_all_packages(void)
{
    DIR *dir = opendir("/var/lib/apt/lists");
    struct dirent *entry;

    if (!dir) return;

    while ((entry = readdir(dir)) != NULL) {
       if (strstr(entry->d_name, "_Packages.gz")) {
          char *path = xasprintf("/var/lib/apt/lists/%s", entry->d_name);
          char *dists = strstr(entry->d_name, "_dists_");
          int len, i;
          char *uri, *repo_uri;

          if (!dists) {
             free(path); continue;
          }

          len = dists - entry->d_name;
          uri = xstrndup(entry->d_name, len);
          for (i = 0; uri[i]; i++) {
             if (uri[i] == '_') uri[i] = '/';
          }
          repo_uri = xasprintf("http://%s", uri);

          parse_package_file(path, repo_uri);

          free(repo_uri);
          free(uri);
          free(path);
       }
    }
    closedir(dir);
    
    /* Convert linked list to sorted array for performance */
    finalize_packages();
}

static int order(char c)
{
    if (isdigit(c)) return 0;
    if (isalpha(c)) return (unsigned char)c;
    if (c == '~') return -1;
    if (c) return (unsigned char)c + 256;
    return 0;
}

static int compare_version_part(const char *v1, const char *v2)
{
    while (*v1 || *v2) {
       int first_diff = 0;
       while ((*v1 && !isdigit(*v1)) || (*v2 && !isdigit(*v2))) {
          int o1 = order(*v1);
          int o2 = order(*v2);
          if (o1 != o2) return o1 - o2;
          if (*v1) v1++;
          if (*v2) v2++;
       }
       while (*v1 == '0') v1++;
       while (*v2 == '0') v2++;
       while (isdigit(*v1) && isdigit(*v2)) {
          if (!first_diff) first_diff = *v1 - *v2;
          v1++; v2++;
       }
       if (isdigit(*v1)) return 1;
       if (isdigit(*v2)) return -1;
       if (first_diff) return first_diff;
    }
    return 0;
}

static int compare_versions(const char *v1, const char *v2)
{
    const char *e1, *e2, *u1, *u2, *r1, *r2;
    long epoch1 = 0, epoch2 = 0;

    if (!v1 || !v2) return v1 ? 1 : (v2 ? -1 : 0);

    e1 = strchr(v1, ':');
    if (e1) { epoch1 = strtol(v1, NULL, 10); u1 = e1 + 1; } 
    else u1 = v1;

    e2 = strchr(v2, ':');
    if (e2) { epoch2 = strtol(v2, NULL, 10); u2 = e2 + 1; } 
    else u2 = v2;

    if (epoch1 != epoch2) return (epoch1 > epoch2) ? 1 : -1;

    r1 = strrchr(u1, '-');
    r2 = strrchr(u2, '-');

    if (r1 && r2) {
       char *up1 = xstrndup(u1, r1 - u1);
       char *up2 = xstrndup(u2, r2 - u2);
       int res = compare_version_part(up1, up2);
       free(up1); free(up2);
       if (res) return res;
       return compare_version_part(r1 + 1, r2 + 1);
    } else if (r1) {
       char *up1 = xstrndup(u1, r1 - u1);
       int res = compare_version_part(up1, u2);
       free(up1);
       if (res) return res;
       return compare_version_part(r1 + 1, "");
    } else if (r2) {
       char *up2 = xstrndup(u2, r2 - u2);
       int res = compare_version_part(u1, up2);
       free(up2);
       if (res) return res;
       return compare_version_part("", r2 + 1);
    }
    return compare_version_part(u1, u2);
}

static int count_upgradable(void)
{
    FILE *f = fopen_for_read("/var/lib/dpkg/status");
    int count = 0;
    char *line, *curr_pkg = NULL, *curr_ver = NULL;
    bool installed = false;

    if (!f) return 0;

    while ((line = xmalloc_fgetline(f)) != NULL) {
       if (strncmp(line, "Package: ", 9) == 0) {
          curr_pkg = xstrdup(skip_whitespace(line + 9));
       } else if (strncmp(line, "Version: ", 9) == 0) {
          curr_ver = xstrdup(skip_whitespace(line + 9));
       } else if (strncmp(line, "Status: ", 8) == 0) {
          if (strstr(line, "installed")) installed = true;
       } else if (*line == '\0') {
          if (installed && curr_pkg && curr_ver) {
             pkg_t *p = find_package(curr_pkg);
             if (p && compare_versions(p->version, curr_ver) > 0) count++;
          }
          free(curr_pkg); curr_pkg = NULL;
          free(curr_ver); curr_ver = NULL;
          installed = false;
       }
       free(line);
    }
    free(curr_pkg);
    free(curr_ver);
    fclose(f);
    return count;
}

int apt_main(int argc, char **argv) MAIN_EXTERNALLY_VISIBLE;
static int check_deps_sanity(pkg_t *p)
{
    int failed_count = 0;
    char *all_deps, *deps_copy, *saveptr, *dep_entry;

    if (!p->depends && !p->pre_depends) return 0;

    all_deps = xasprintf("%s%s%s",
       p->pre_depends ? p->pre_depends : "",
       (p->pre_depends && p->depends) ? ", " : "",
       p->depends ? p->depends : "");

    deps_copy = xstrdup(all_deps);
    dep_entry = strtok_r(deps_copy, ",", &saveptr);

    while (dep_entry) {
       char *or_saveptr, *alt;
       char *dep_entry_copy = xstrdup(dep_entry);
       bool satisfied = false;

       alt = strtok_r(dep_entry_copy, "|", &or_saveptr);
       while (alt) {
          char *clean_alt = skip_whitespace(alt);
          char *p_spec = strpbrk(clean_alt, " (:");
          if (p_spec) *p_spec = '\0';

          if (is_installed(clean_alt)) { satisfied = true; break; }
          alt = strtok_r(NULL, "|", &or_saveptr);
       }

       if (!satisfied) {
          printf("    %s[ERROR]%s Missing dependency: %s\n", CLR_RED, CLR_RESET, dep_entry);
          failed_count++;
       }
       free(dep_entry_copy);
       dep_entry = strtok_r(NULL, ",", &saveptr);
    }
    free(deps_copy);
    free(all_deps);
    return failed_count;
}

static int check_files_sanity(const char *pkg_name)
{
    int failed_count = 0;
    char *list_file = xasprintf("/var/lib/dpkg/info/%s.list", pkg_name);
    FILE *f = fopen(list_file, "r");
    char *line;

    if (!f) {
       free(list_file);
       list_file = xasprintf("/var/lib/dpkg/info/%s:%s.list", pkg_name, get_debian_arch());
       f = fopen(list_file, "r");
    }

    if (!f) {
       printf("    %s[WARN]%s Could not find file list (.list)\n", CLR_RED, CLR_RESET);
       free(list_file);
       return 1;
    }

    while ((line = xmalloc_fgetline(f)) != NULL) {
       struct stat st;
       if (lstat(line, &st) != 0) {
          printf("    %s[MISSING]%s %s\n", CLR_RED, CLR_RESET, line);
          failed_count++;
       } else if (S_ISLNK(st.st_mode)) {
          struct stat st2;
          if (stat(line, &st2) != 0) {
             printf("    %s[BROKEN LINK]%s %s\n", CLR_RED, CLR_RESET, line);
             failed_count++;
          }
       }
       free(line);
    }
    fclose(f);
    free(list_file);
    return failed_count;
}

static bool has_enough_disk_space(long required_kb)
{
    struct statvfs st;
    unsigned long long free_kb;
    unsigned long long safe_required_kb;

    /* Check the root filesystem '/' (or change to '/var' if using separate partitions) */
    if (statvfs("/", &st) != 0) {
        bb_perror_msg("Failed to stat filesystem to check disk space");
        /* Fail-safe: if we can't read the disk space, don't allow massive installs */
        return false;
    }

    /* f_bavail represents free blocks available to non-root users.
       Multiply by fragment size to get bytes, then divide by 1024 for kB */
    free_kb = ((unsigned long long)st.f_bavail * st.f_frsize) / 1024;

    /* Add a 50MB (51200 kB) safety buffer so we don't completely brick the OS */
    safe_required_kb = (required_kb > 0 ? (unsigned long long)required_kb : 0) + 51200;

    printf("Storage protection: %llu kB available", free_kb);

    if (free_kb < safe_required_kb) {
        printf(" %s[FAILED]%s\n", CLR_RED, CLR_RESET);
        printf("  %s[FATAL ERROR]%s Insufficient disk space!\n", CLR_RED, CLR_RESET);
        printf("  Required: %llu kB (including 50MB safety buffer)\n", safe_required_kb);
        return false;
    }

    printf(" %s[PASSED]%s\n", CLR_GREEN, CLR_RESET);
    return true;
}

static bool is_valid_pkg_name(const char *name)
{
    int i;
    for (i = 0; name[i]; i++) {
        if (!isalnum(name[i]) && name[i] != '-' && name[i] != '+' && name[i] != '.')
            return false;
    }
    return true;
}

int apt_main(int argc, char **argv)
{
    const char *cmd = NULL;
    bool force_depends = false;
    int i, new_argc = 0;
    char **new_argv;

    if (argc < 2) bb_show_usage();

    new_argv = xzalloc(sizeof(char *) * (argc + 1));

    for (i = 1; i < argc; i++) {
       if (argv[i][0] == '-') {
          if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fix-broken") == 0) {
             force_depends = true;
          } else {
             new_argv[new_argc++] = argv[i];
          }
       } else if (!cmd) {
          cmd = argv[i];
       } else {
          new_argv[new_argc++] = argv[i];
       }
    }

    if (!cmd) bb_show_usage();

    setup_common_bufsiz();
    memset(&G, 0, sizeof(G));
    get_bb_exe(G.bb_path, sizeof(G.bb_path));

    argv = new_argv;
    argc = new_argc;

    if (strcmp(cmd, "update") == 0) {
       repo_t *repos, *r;
       int upgradable, hit_count = 1;

       repos = parse_sources_list("/etc/apt/sources.list");
       if (!repos) bb_error_msg_and_die("could not parse /etc/apt/sources.list");

       for (r = repos; r; r = r->next) {
          printf("Hit:%d %s %s InRelease\n", hit_count++, r->uri, r->dist);
       }

       printf("Reading package lists... Done\n");
       update_repos(repos);

       load_all_packages();
       upgradable = count_upgradable();
       printf("Building dependency tree... Done\n");
       printf("Reading state information... Done\n");
       if (upgradable > 0)
          printf("%d packages can be upgraded. Run 'apt list --upgradable' to see them.\n", upgradable);

    } else if (strcmp(cmd, "install") == 0 || strcmp(cmd, "reinstall") == 0 || strcmp(cmd, "rescue-install") == 0 || strcmp(cmd, "upgrade") == 0) {
       bool is_reinstall = (strcmp(cmd, "reinstall") == 0);
       bool is_rescue = (strcmp(cmd, "rescue-install") == 0);
       bool is_upgrade = (strcmp(cmd, "upgrade") == 0);
       llist_t *curr, *recommends_list = NULL;
       int count = 0, total;
       long total_size = 0, total_installed_size = 0;
       unsigned w, h;
       char **dpkg_argv;
       int arg_idx;

       printf("Reading package lists... Done\n");
       load_all_packages();
       load_installed_packages();
       printf("Building dependency tree... Done\n");
       printf("Reading state information... Done\n");

       if (is_upgrade) {
           FILE *f;
           printf("Calculating upgrade... Done\n");
           f = fopen_for_read("/var/lib/dpkg/status");
           if (f) {
              char *line, *curr_pkg = NULL, *curr_ver = NULL;
              bool installed = false;

              while ((line = xmalloc_fgetline(f)) != NULL) {
                 if (strncmp(line, "Package: ", 9) == 0) curr_pkg = xstrdup(skip_whitespace(line + 9));
                 else if (strncmp(line, "Version: ", 9) == 0) curr_ver = xstrdup(skip_whitespace(line + 9));
                 else if (strncmp(line, "Status: ", 8) == 0 && strstr(line, "installed")) installed = true;
                 else if (*line == '\0') {
                    if (installed && curr_pkg && curr_ver) {
                       pkg_t *p = find_package(curr_pkg);
                       if (p && compare_versions(p->version, curr_ver) > 0) resolve_deps(p, true, 0);
                    }
                    free(curr_pkg); curr_pkg = NULL;
                    free(curr_ver); curr_ver = NULL;
                    installed = false;
                 }
                 free(line);
              }
              if (installed && curr_pkg && curr_ver) {
                 pkg_t *p = find_package(curr_pkg);
                 if (p && compare_versions(p->version, curr_ver) > 0) resolve_deps(p, true, 0);
              }
              free(curr_pkg); free(curr_ver);
              fclose(f);
           }
       } else {
           for (i = 0; i < argc; i++) {
              pkg_t *p = find_package(argv[i]);
              if (!p) { bb_error_msg("E: Unable to locate package %s", argv[i]); continue; }
              if (is_reinstall || is_rescue || !is_installed(argv[i])) resolve_deps(p, true, 0);
              else bb_info_msg("%s is already the newest version.", argv[i]);
           }
       }

       if (!G.install_queue) {
          bb_simple_info_msg("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.");
          return EXIT_SUCCESS;
       }

       for (curr = G.install_queue; curr; curr = curr->link) {
          pkg_t *p = (pkg_t *)curr->data;
          if (p->recommends) {
             char *rec_copy = xstrdup(p->recommends);
             char *saveptr, *token = strtok_r(rec_copy, ",", &saveptr);
             while (token) {
                char *clean = skip_whitespace(token);
                char *p_spec = strpbrk(clean, " (");
                if (p_spec) *p_spec = '\0';
                if (!is_installed(clean)) llist_add_to(&recommends_list, xstrdup(clean));
                token = strtok_r(NULL, ",", &saveptr);
             }
             free(rec_copy);
          }
       }

       if (recommends_list) {
          printf("Note: Recommended packages are NOT installed by default.\nRecommended packages:\n ");
          for (curr = recommends_list; curr; curr = curr->link) printf(" %s", (char *)curr->data);
          printf("\n");
          llist_free(recommends_list, free);
       }

       printf("The following %spackages will be %s:\n ", CLR_BOLD, is_upgrade ? "upgraded" : "installed", CLR_RESET);
       for (curr = G.install_queue; curr; curr = curr->link) {
          pkg_t *p = (pkg_t *)curr->data;
          printf(" %s", p->name);
          count++;
          total_size += p->size;
          total_installed_size += p->installed_size - get_installed_size(p->name);
       }
       
       if (is_upgrade) printf("\n%d upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", count);
       else printf("\n0 upgraded, %d newly installed, 0 to remove and 0 not upgraded.\n", count);

       printf("Need to get %ld kB of archives.\n", total_size / 1024);
       if (total_installed_size >= 0) printf("After this operation, %ld kB of additional disk space will be used.\n", total_installed_size);
       else printf("After this operation, %ld kB disk space will be freed.\n", -total_installed_size);
       
       if (!has_enough_disk_space(total_installed_size > 0 ? (total_installed_size + (total_size / 1024)) : (total_size / 1024))) {
           bb_error_msg_and_die("Installation aborted to protect filesystem integrity.");
       }

       printf("%sDo you want to continue? [Y/n]%s ", CLR_BOLD, CLR_RESET);
       fflush(stdout);

       if (!bb_ask_y_confirmation()) return EXIT_SUCCESS;

       total = llist_count(G.install_queue);
       get_terminal_width_height(STDOUT_FILENO, &w, &h);
       printf("\033[1;%dr", h - 1);

       /* Prepare dynamic arguments array for dpkg to avoid fragmentation */
       dpkg_argv = xmalloc(sizeof(char *) * (total + 5));
       arg_idx = 0;
       dpkg_argv[arg_idx++] = (char*)"dpkg"; /* Placed for alignment, dpkg_call rewrites index 0 */
       dpkg_argv[arg_idx++] = (char*)"-i";
       if (force_depends) dpkg_argv[arg_idx++] = (char*)"--force-depends";

       count = 1;
       for (curr = G.install_queue; curr; curr = curr->link) {
          pkg_t *p = (pkg_t *)curr->data;
          char *url = xasprintf("%s/%s", p->repo_uri, p->filename);
          char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);

          update_progress(count - 1, total, p->name);
          printf("\033[%d;1H", h - 1);
          printf("%sGet:%d%s %s %s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, p->repo_uri, p->name, p->version, p->filename);

          if (download_file(url, tmp_deb) == 0) {
              dpkg_argv[arg_idx++] = tmp_deb;
          }
          free(url);
       }
       update_progress(total, total, "Done");
       dpkg_argv[arg_idx] = NULL; /* Null-terminate array for execvp */

       if (is_rescue) {
          printf("\033[%d;1H", h - 1);
          bb_info_msg("Rescue Mode: Manually extracting packages...");
          for (curr = G.install_queue; curr; curr = curr->link) {
             pkg_t *p = (pkg_t *)curr->data;
             char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
             
             if (is_valid_pkg_name(p->name)) {
                char *r_cmd;
                printf("  Extracting %s...\n", p->name);
                /* system() is acceptable here because package name is strictly validated */
                r_cmd = xasprintf("ar -p %s data.tar.xz 2>/dev/null | tar -C / -xJ 2>/dev/null || "
                                 "ar -p %s data.tar.gz 2>/dev/null | tar -C / -xz 2>/dev/null",
                                 tmp_deb, tmp_deb);
                system(r_cmd);
                free(r_cmd);
             } else {
                bb_error_msg("Invalid package name syntax for %s, skipping rescue extraction", p->name);
             }
             free(tmp_deb);
          }
       } else if (arg_idx > 3) { /* Ensure we actually downloaded something */
          printf("\033[%d;1H", h - 1);
          bb_info_msg("Configuring packages...");
          dpkg_call(dpkg_argv);

          /* Cleanup */
          for (curr = G.install_queue; curr; curr = curr->link) {
             pkg_t *p = (pkg_t *)curr->data;
             char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
             unlink(tmp_deb);
             free(tmp_deb);
          }
       }
       
       printf("\033[r\033[%d;1H\n", h);
       free(dpkg_argv);

    } else if (strcmp(cmd, "remove") == 0) {
       char **temp_argv;

       if (argc == 0) bb_show_usage();

       printf("The following packages will be REMOVED:\n ");
       temp_argv = argv;
       while (*temp_argv) { printf(" %s", *temp_argv); temp_argv++; }
       printf("\nDo you want to continue? [Y/n] ");
       fflush(stdout);

       if (!bb_ask_y_confirmation()) return EXIT_SUCCESS;

       while (*argv) {
          char *dpkg_args_rm[] = {(char*)"dpkg", (char*)"-r", *argv, NULL};
          bb_info_msg("Removing %s...", *argv);
          dpkg_call(dpkg_args_rm);
          argv++;
       }

    } else if (strcmp(cmd, "list") == 0) {
       if (argc > 0 && strcmp(argv[0], "--upgradable") == 0) {
          FILE *f;
          load_all_packages();
          printf("Listing... Done\n");
          f = fopen_for_read("/var/lib/dpkg/status");
          if (f) {
             char *line, *curr_pkg = NULL, *curr_ver = NULL;
             bool installed = false;
             while ((line = xmalloc_fgetline(f)) != NULL) {
                if (strncmp(line, "Package: ", 9) == 0) curr_pkg = xstrdup(skip_whitespace(line + 9));
                else if (strncmp(line, "Version: ", 9) == 0) curr_ver = xstrdup(skip_whitespace(line + 9));
                else if (strncmp(line, "Status: ", 8) == 0 && strstr(line, "installed")) installed = true;
                else if (*line == '\0') {
                   if (installed && curr_pkg && curr_ver) {
                      pkg_t *p = find_package(curr_pkg);
                      if (p && compare_versions(p->version, curr_ver) > 0) {
                         printf("%s/%s %s %s [upgradable from: %s]\n", p->name, "stable", p->version, get_debian_arch(), curr_ver);
                      }
                   }
                   free(curr_pkg); curr_pkg = NULL;
                   free(curr_ver); curr_ver = NULL;
                   installed = false;
                }
                free(line);
             }
             fclose(f);
          }
       } else {
          printf("Use --upgradable to see packages that can be upgraded.\n");
       }

    } else if (strcmp(cmd, "search") == 0) {
       pkg_t *p;
       pkg_t **results = NULL;
       int res_count = 0, search_idx;

       if (argc == 0) bb_show_usage();
       load_all_packages();

       /* Collect results */
       for (i = 0; i < G.pkg_count; i++) {
          p = G.pkg_array[i];
          for (search_idx = 0; search_idx < argc; search_idx++) {
             if (strstr(p->name, argv[search_idx]) || (p->description && strstr(p->description, argv[search_idx]))) {
                results = xrealloc_vector(results, 3, res_count);
                results[res_count++] = p;
                break;
             }
          }
       }

       if (res_count > 0) {
          bb_simple_info_msg("Sorting...");
          /* Safe O(N log N) sorting using standard qsort */
          qsort(results, res_count, sizeof(pkg_t *), cmp_pkg);

          for (i = 0; i < res_count; i++) {
             p = results[i];
             printf("%s%s%s/%s %s %s\n", CLR_GREEN, p->name, CLR_RESET, "stable", p->version, p->repo_uri);
             if (p->description) {
                char *short_desc = xstrdup(p->description);
                char *nl = strchr(short_desc, '\n');
                if (nl) *nl = '\0';
                printf("  %s\n", short_desc);
                free(short_desc);
             }
          }
          free(results);
       }

    } else if (strcmp(cmd, "verify") == 0) {
       if (argc == 0) bb_show_usage();
       load_all_packages();
       load_installed_packages();
       
       for (i = 0; i < argc; i++) {
          pkg_t *p = find_package(argv[i]);
          bool installed = is_installed(argv[i]);
          int total_failed = 0;

          printf("%sVerifying package:%s %s\n", CLR_BOLD, CLR_RESET, argv[i]);
          printf("  [INFO] Status:  %s%s%s\n", installed ? CLR_GREEN : CLR_RED, installed ? "Installed" : "NOT INSTALLED / BROKEN", CLR_RESET);

          if (p) {
             printf("  [INFO] Version: %s\n", p->version);
             if (installed) {
                int d_failed, f_failed;
                printf("  [CHECK] Dependencies... ");
                fflush(stdout);
                d_failed = check_deps_sanity(p);
                if (d_failed == 0) printf("%sOK%s\n", CLR_GREEN, CLR_RESET);
                else printf("%sFAILED (%d missing)%s\n", CLR_RED, d_failed, CLR_RESET);

                printf("  [CHECK] Filesystem... ");
                fflush(stdout);
                f_failed = check_files_sanity(argv[i]);
                if (f_failed == 0) printf("%sOK%s\n", CLR_GREEN, CLR_RESET);
                else printf("%sFAILED (%d issues)%s\n", CLR_RED, f_failed, CLR_RESET);

                total_failed = d_failed + f_failed;
             } else total_failed = 1;
          } else {
             printf("  [WARN] Package metadata not found in cache.\n");
             total_failed = 1;
          }

          printf("  [RESULT] Sanity check: %s%s%s\n\n", total_failed == 0 ? CLR_GREEN : CLR_RED, total_failed == 0 ? "PASSED" : "FAILED", CLR_RESET);
       }

    } else if (strcmp(cmd, "md5check") == 0) {
       if (argc == 0) bb_show_usage();
       
       for (i = 0; i < argc; i++) {
          char *sums_file = xasprintf("/var/lib/dpkg/info/%s.md5sums", argv[i]);
          FILE *f = fopen(sums_file, "r");

          if (!f) {
             free(sums_file);
             sums_file = xasprintf("/var/lib/dpkg/info/%s:%s.md5sums", argv[i], get_debian_arch());
             f = fopen(sums_file, "r");
          }

          if (!f) {
             bb_error_msg("no md5sums for %s", argv[i]);
             free(sums_file); continue;
          }

          printf("Verifying %s...\n", argv[i]);
          {
             char *line;
             int checked = 0, failed = 0;
             while ((line = xmalloc_fgetline(f)) != NULL) {
                char *md5 = strtok(line, " \t");
                char *fname = strtok(NULL, " \t");
                if (md5 && fname) {
                   char *path = (fname[0] == '/') ? xstrdup(fname) : xasprintf("/%s", fname);
                   if (md5_check(md5, path) != 0) {
                      printf("%s%s: FAILED%s\n", CLR_RED, path, CLR_RESET);
                      failed++;
                   }
                   checked++;
                   free(path);
                }
                free(line);
             }
             if (failed == 0) printf("%sVerification successful:%s all %d files match md5sums.\n", CLR_GREEN, CLR_RESET, checked);
             else printf("%sVerification failed:%s %d of %d files are corrupted.\n", CLR_RED, CLR_RESET, failed, checked);
          }
          fclose(f);
          free(sums_file);
       }
    } else bb_show_usage();

    return EXIT_SUCCESS;
}
