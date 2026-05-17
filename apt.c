/* vi: set sw=4 ts=4: */
/*
 * busybox_apt: A lightweight implementation of APT for BusyBox.
 *
 * Licensed under GPLv2 or later, see file LICENSE in this source tree.
 */

//config:config APT
//config:	bool "apt (8 kb)"
//config:	default y
//config:	select DPKG
//config:	help
//config:	apt is a high-level package manager for Debian-based systems.
//config:	It provides repository management and dependency resolution
//config:	as a frontend to dpkg.

//applet:IF_APT(APPLET(apt, BB_DIR_USR_BIN, BB_SUID_DROP))

//kbuild:lib-$(CONFIG_APT) += apt.o

//usage:#define apt_trivial_usage
//usage:       "[-f] COMMAND [PACKAGE...]"
//usage:#define apt_full_usage "\n\n"
//usage:       "High-level package manager\n"
//usage:     "\nOptions:"
//usage:     "\n	-f, --fix-broken	Pass --force-depends to dpkg"
//usage:     "\n\nCommands:"
//usage:     "\n	update		Update list of available packages"
//usage:     "\n	install		Install new packages"
//usage:     "\n	remove		Remove packages"
//usage:     "\n	upgrade		Upgrade the system"
//usage:     "\n	list --upgradable	List upgradable packages"
//usage:     "\n	search		Search for a package"

#include "libbb.h"
#include <sys/utsname.h>

/* ANSI Colors */
#define CLR_GREEN  "\033[0;32m"
#define CLR_BOLD   "\033[1m"
#define CLR_RESET  "\033[0m"
#define CLR_BG_GREEN "\033[42m"
#define CLR_BG_GRAY  "\033[47m"

/* Progress bar constants */
#define BAR_WIDTH 25

static void update_progress(int current, int total, const char *msg UNUSED_PARAM)
{
	unsigned width, height;
	int i, pos, percent;
	int bar_space;

	get_terminal_width_height(STDOUT_FILENO, &width, &height);
	if (width < 30) return;

	percent = (current * 100) / total;
	/* Width minus overhead for "Progress: [100%] [" (18) and "]" (1) and safety (5) */
	bar_space = width - 24;
	if (bar_space < 10) bar_space = 10;
	pos = (percent * bar_space) / 100;

	/* Move to the absolute bottom line (outside scrolling region) */
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

typedef struct repo_s {
	char *uri;
	char *dist;
	llist_t *components;
	struct repo_s *next;
} repo_t;

static repo_t *parse_sources_list(const char *filename)
{
	FILE *f;
	repo_t *repos = NULL;
	char *line;

	f = fopen_for_read(filename);
	if (!f) return NULL;

	while ((line = xmalloc_fgetline(f)) != NULL) {
		char *ptr = line;
		char *type, *uri, *dist;

		/* Skip whitespace */
		ptr = skip_whitespace(ptr);
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
	char *s;
	char *ret;

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
			char *wget_cmd;

			printf("%sGet:%d%s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, url, (char *)comp->data);

			/* Use wget applet to download */
			wget_cmd = xasprintf("wget -q -O %s %s", local_path, url);
			if (system(wget_cmd) != 0) {
				/* bb_error_msg("failed to download %s", url); */
			}

			free(wget_cmd);
			free(local_path);
			free(local_name);
			free(url);
			comp = comp->link;
		}
		free(uri_part);
		curr = curr->next;
	}
}

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
	int state; /* 0: unvisited, 1: queued, 2: installing */
	struct pkg_s *next;
} pkg_t;

static pkg_t *all_packages = NULL;
static llist_t *install_queue = NULL;
static llist_t *installed_packages = NULL;

static void add_package(pkg_t *pkg)
{
	pkg->next = all_packages;
	all_packages = pkg;
}

static pkg_t *find_package(const char *name)
{
	pkg_t *p = all_packages;
	while (p) {
		if (strcmp(p->name, name) == 0) return p;
		/* Also check Provides */
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
		p = p->next;
	}
	return NULL;
}

static bool is_installed(const char *pkg_name)
{
	llist_t *curr = installed_packages;
	while (curr) {
		if (strcmp((char *)curr->data, pkg_name) == 0) return true;
		curr = curr->link;
	}
	return false;
}

static void load_installed_packages(void)
{
	FILE *f;
	char *line;
	char *curr_pkg = NULL;
	bool installed = false;

	f = fopen_for_read("/var/lib/dpkg/status");
	if (!f) return;

	while ((line = xmalloc_fgetline(f)) != NULL) {
		if (strncmp(line, "Package: ", 9) == 0) {
			curr_pkg = xstrdup(skip_whitespace(line + 9));
		} else if (strncmp(line, "Status: ", 8) == 0) {
			if (strstr(line, "installed")) installed = true;
		} else if (strncmp(line, "Provides: ", 10) == 0) {
			if (installed && curr_pkg) {
				char *prov_copy = xstrdup(skip_whitespace(line + 10));
				char *saveptr;
				char *token = strtok_r(prov_copy, ",", &saveptr);
				while (token) {
					char *clean = skip_whitespace(token);
					char *p_spec = strpbrk(clean, " (:");
					if (p_spec) *p_spec = '\0';
					llist_add_to(&installed_packages, xstrdup(clean));
					token = strtok_r(NULL, ",", &saveptr);
				}
				free(prov_copy);
			}
		} else if (*line == '\0') {
			if (installed && curr_pkg) {
				llist_add_to(&installed_packages, curr_pkg);
				curr_pkg = NULL;
			}
			free(curr_pkg); curr_pkg = NULL;
			installed = false;
		}
		free(line);
	}
	free(curr_pkg);
	fclose(f);
}

static void resolve_deps(pkg_t *p)
{
	char *deps_copy;
	char *saveptr;
	char *dep_entry;

	if (!p || p->state != 0) return;
	if (is_installed(p->name)) return;

	p->state = 1; /* Mark as queued to avoid loops */

	/* Handle Pre-Depends first */
	if (p->pre_depends) {
		char *pre_copy = xstrdup(p->pre_depends);
		char *pre_saveptr;
		char *pre_entry = strtok_r(pre_copy, ",", &pre_saveptr);
		while (pre_entry) {
			char *or_saveptr;
			char *alt;
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
				if (to_install) resolve_deps(to_install);
			}
			pre_entry = strtok_r(NULL, ",", &pre_saveptr);
		}
		free(pre_copy);
	}

	if (p->depends) {
		deps_copy = xstrdup(p->depends);
		dep_entry = strtok_r(deps_copy, ",", &saveptr);

		while (dep_entry) {
			char *or_saveptr;
			char *alt;
			char *dep_entry_copy;
			pkg_t *to_install = NULL;
			bool satisfied = false;

			/* Check if any alternative is already satisfied */
			dep_entry_copy = xstrdup(dep_entry);
			alt = strtok_r(dep_entry_copy, "|", &or_saveptr);
			while (alt) {
				char *clean_alt = skip_whitespace(alt);
				char *p_spec = strpbrk(clean_alt, " (:");
				if (p_spec) *p_spec = '\0';

				if (is_installed(clean_alt)) {
					satisfied = true;
					break;
				}
				alt = strtok_r(NULL, "|", &or_saveptr);
			}
			free(dep_entry_copy);

			if (!satisfied) {
				/* Try to find the first available alternative in repositories */
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

				if (to_install) {
					resolve_deps(to_install);
				}
			}
			dep_entry = strtok_r(NULL, ",", &saveptr);
		}
		free(deps_copy);
	}

	llist_add_to_end(&install_queue, p);
}

static void parse_package_file(const char *filename, const char *repo_uri)
{
	/* Need to handle .gz files. BusyBox has zcat. */
	char *cmd = xasprintf("zcat %s", filename);
	FILE *f = popen(cmd, "r");
	char *line;
	pkg_t *curr = NULL;
	char **last_field = NULL;

	if (!f) {
		free(cmd);
		return;
	}

	while ((line = xmalloc_fgetline(f)) != NULL) {
		if (*line == '\0') {
			if (curr) {
				add_package(curr);
				curr = NULL;
			}
			last_field = NULL;
		} else if (*line == ' ' || *line == '\t') {
			/* Continuation of previous field */
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
		} else {
			last_field = NULL;
		}
		free(line);
	}
	if (curr) add_package(curr);
	pclose(f);
	free(cmd);
}

static void load_all_packages(void)
{
	/* We need to map list files back to their URIs.
	 * A better way is to store the mapping during 'update' or parse it from filenames.
	 * Filenames are encoded URIs. Let's decode them simple way. */
	DIR *dir = opendir("/var/lib/apt/lists");
	struct dirent *entry;

	if (!dir) return;

	while ((entry = readdir(dir)) != NULL) {
		if (strstr(entry->d_name, "_Packages.gz")) {
			char *path = xasprintf("/var/lib/apt/lists/%s", entry->d_name);
			char *dists = strstr(entry->d_name, "_dists_");
			int len;
			char *uri;
			char *repo_uri;
			int i;

			if (!dists) {
				free(path);
				continue;
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
}

/* Robust Debian-style version comparison */
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
	const char *e1, *e2;
	long epoch1 = 0, epoch2 = 0;
	const char *u1, *u2;
	const char *r1, *r2;

	if (!v1 || !v2) return v1 ? 1 : (v2 ? -1 : 0);

	e1 = strchr(v1, ':');
	if (e1) {
		epoch1 = strtol(v1, NULL, 10);
		u1 = e1 + 1;
	} else {
		u1 = v1;
	}

	e2 = strchr(v2, ':');
	if (e2) {
		epoch2 = strtol(v2, NULL, 10);
		u2 = e2 + 1;
	} else {
		u2 = v2;
	}

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
	FILE *f;
	int count = 0;
	char *line;
	char *curr_pkg = NULL;
	char *curr_ver = NULL;
	bool installed = false;

	f = fopen_for_read("/var/lib/dpkg/status");
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
				if (p && compare_versions(p->version, curr_ver) > 0) {
					count++;
				}
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
int apt_main(int argc, char **argv)
{
	const char *cmd = NULL;
	const char *dpkg_force = "";
	int i;
	int new_argc = 0;
	char **new_argv;

	if (argc < 2)
		bb_show_usage();

	new_argv = xzalloc(sizeof(char *) * (argc + 1));

	/* First pass: extract options and command */
	for (i = 1; i < argc; i++) {
		if (argv[i][0] == '-') {
			if (strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--fix-broken") == 0) {
				dpkg_force = " --force-depends";
			} else {
				/* Other options go into new_argv for subcommand specific options */
				new_argv[new_argc++] = argv[i];
			}
		} else if (!cmd) {
			cmd = argv[i];
		} else {
			new_argv[new_argc++] = argv[i];
		}
	}

	if (!cmd)
		bb_show_usage();

	argv = new_argv;
	argc = new_argc;

	if (strcmp(cmd, "update") == 0) {
		repo_t *repos, *r;
		int upgradable;
		int hit_count = 1;

		repos = parse_sources_list("/etc/apt/sources.list");
		if (!repos) {
			bb_error_msg_and_die("could not parse /etc/apt/sources.list");
		}

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
	} else if (strcmp(cmd, "install") == 0) {
		llist_t *curr;
		int count;
		long total_size;
		char *dpkg_args;
		llist_t *recommends_list = NULL;
		int total;
		unsigned w, h;

		printf("Reading package lists... Done\n");
		load_all_packages();
		load_installed_packages();
		printf("Building dependency tree... Done\n");
		printf("Reading state information... Done\n");

		for (i = 0; i < argc; i++) {
			if (is_installed(argv[i])) {
				bb_info_msg("%s is already the newest version.", argv[i]);
			} else {
				pkg_t *p = find_package(argv[i]);
				if (p) {
					resolve_deps(p);
				} else {
					bb_error_msg("E: Unable to locate package %s", argv[i]);
				}
			}
		}

		curr = install_queue;
		if (!curr) {
			bb_simple_info_msg("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.");
			return EXIT_SUCCESS;
		}

		/* Collect recommendations from all queued packages */
		for (curr = install_queue; curr; curr = curr->link) {
			pkg_t *p = (pkg_t *)curr->data;
			if (p->recommends) {
				char *rec_copy = xstrdup(p->recommends);
				char *saveptr;
				char *token = strtok_r(rec_copy, ",", &saveptr);
				while (token) {
					char *clean = skip_whitespace(token);
					char *p_spec = strpbrk(clean, " (");
					if (p_spec) *p_spec = '\0';
					if (!is_installed(clean)) {
						llist_add_to(&recommends_list, xstrdup(clean));
					}
					token = strtok_r(NULL, ",", &saveptr);
				}
				free(rec_copy);
			}
		}

		if (recommends_list) {
			printf("Note: Recommended packages are NOT installed by default.\n");
			printf("Recommended packages:\n ");
			for (curr = recommends_list; curr; curr = curr->link) {
				printf(" %s", (char *)curr->data);
			}
			printf("\n");
			llist_free(recommends_list, free);
		}

		printf("The following %sNEW%s packages will be installed:\n ", CLR_BOLD, CLR_RESET);
		count = 0;
		total_size = 0;
		for (curr = install_queue; curr; curr = curr->link) {
			pkg_t *p = (pkg_t *)curr->data;
			printf(" %s", p->name);
			count++;
			total_size += p->size;
		}
		printf("\n0 upgraded, %d newly installed, 0 to remove and 0 not upgraded.\n", count);
		printf("Need to get %ld kB of archives.\n", total_size / 1024);
		printf("After this operation, 0 B of additional disk space will be used.\n");
		printf("%sDo you want to continue? [Y/n]%s ", CLR_BOLD, CLR_RESET);
		fflush(stdout);

		if (!bb_ask_y_confirmation())
			return EXIT_SUCCESS;

		dpkg_args = xstrdup("");
		count = 1;
		total = llist_count(install_queue);

		/* Set scrolling region to leave bottom line for progress */
		get_terminal_width_height(STDOUT_FILENO, &w, &h);
		printf("\033[1;%dr", h - 1);

		for (curr = install_queue; curr; curr = curr->link) {
			pkg_t *p = (pkg_t *)curr->data;
			char *url = xasprintf("%s/%s", p->repo_uri, p->filename);
			char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
			char *wget_cmd;
			char *new_args;

			update_progress(count - 1, total, p->name);
			printf("\033[%d;1H", h - 1); /* Move just above progress bar */
			printf("%sGet:%d%s %s %s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, p->repo_uri, p->name, p->version, p->filename);

			wget_cmd = xasprintf("wget -q -O %s %s", tmp_deb, url);
			if (system(wget_cmd) == 0) {
				new_args = xasprintf("%s %s", dpkg_args, tmp_deb);
				free(dpkg_args);
				dpkg_args = new_args;
			}
			free(wget_cmd);
			free(url);
		}
		update_progress(total, total, "Done");

		if (*dpkg_args) {
			char *dpkg_cmd = xasprintf("dpkg%s -i %s", dpkg_force, dpkg_args);
			printf("\033[%d;1H", h - 1);
			bb_info_msg("Configuring packages...");
			system(dpkg_cmd);
			free(dpkg_cmd);

			/* Now cleanup */
			curr = install_queue;
			while (curr) {
				pkg_t *p = (pkg_t *)curr->data;
				char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
				unlink(tmp_deb);
				free(tmp_deb);
				curr = curr->link;
			}
		}
		/* Reset scrolling region */
		printf("\033[r\033[%d;1H\n", h);
		free(dpkg_args);
	} else if (strcmp(cmd, "remove") == 0) {
		char **temp_argv;

		if (argc == 0) bb_show_usage();

		printf("The following packages will be REMOVED:\n ");
		temp_argv = argv;
		while (*temp_argv) {
			printf(" %s", *temp_argv);
			temp_argv++;
		}
		printf("\nDo you want to continue? [Y/n] ");
		fflush(stdout);

		if (!bb_ask_y_confirmation())
			return EXIT_SUCCESS;

		while (*argv) {
			char *dpkg_cmd;
			bb_info_msg("Removing %s...", *argv);
			dpkg_cmd = xasprintf("dpkg -r %s", *argv);
			system(dpkg_cmd);
			free(dpkg_cmd);
			argv++;
		}
	} else if (strcmp(cmd, "upgrade") == 0) {
		FILE *f;
		llist_t *curr;
		int count;
		long total_size;
		char *dpkg_args;
		llist_t *recommends_list = NULL;
		int total_upg;
		unsigned w_upg, h_upg;

		printf("Reading package lists... Done\n");
		load_all_packages();
		load_installed_packages();
		printf("Building dependency tree... Done\n");
		printf("Reading state information... Done\n");
		printf("Calculating upgrade... Done\n");

		f = fopen_for_read("/var/lib/dpkg/status");
		if (f) {
			char *line;
			char *curr_pkg = NULL;
			char *curr_ver = NULL;
			bool installed = false;

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
						if (p && compare_versions(p->version, curr_ver) > 0) {
							resolve_deps(p);
						}
					}
					free(curr_pkg); curr_pkg = NULL;
					free(curr_ver); curr_ver = NULL;
					installed = false;
				}
				free(line);
			}
			if (installed && curr_pkg && curr_ver) {
				pkg_t *p = find_package(curr_pkg);
				if (p && compare_versions(p->version, curr_ver) > 0) {
					resolve_deps(p);
				}
			}
			free(curr_pkg);
			free(curr_ver);
			fclose(f);
		}

		curr = install_queue;
		if (!curr) {
			bb_simple_info_msg("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.");
			return EXIT_SUCCESS;
		}

		/* Collect recommendations */
		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			if (p->recommends) {
				char *rec_copy = xstrdup(p->recommends);
				char *saveptr;
				char *token = strtok_r(rec_copy, ",", &saveptr);
				while (token) {
					char *clean = skip_whitespace(token);
					char *p_spec = strpbrk(clean, " (");
					if (p_spec) *p_spec = '\0';
					if (!is_installed(clean)) {
						llist_add_to(&recommends_list, xstrdup(clean));
					}
					token = strtok_r(NULL, ",", &saveptr);
				}
				free(rec_copy);
			}
			curr = curr->link;
		}

		if (recommends_list) {
			printf("Note: Recommended packages are NOT installed by default.\n");
			printf("Recommended packages:\n ");
			for (curr = recommends_list; curr; curr = curr->link) {
				printf(" %s", (char *)curr->data);
			}
			printf("\n");
			llist_free(recommends_list, free);
		}

		total_size = 0;
		printf("The following packages will be upgraded:\n ");
		curr = install_queue;
		count = 0;
		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			printf(" %s", p->name);
			count++;
			total_size += p->size;
			curr = curr->link;
		}
		printf("\n%d upgraded, 0 newly installed, 0 to remove and 0 not upgraded.\n", count);
		printf("Need to get %ld kB of archives.\n", total_size / 1024);
		printf("After this operation, 0 B of additional disk space will be used.\n");
		printf("%sDo you want to continue? [Y/n]%s ", CLR_BOLD, CLR_RESET);
		fflush(stdout);

		if (!bb_ask_y_confirmation())
			return EXIT_SUCCESS;

		dpkg_args = xstrdup("");
		curr = install_queue;
		count = 1;
		total_upg = llist_count(install_queue);

		get_terminal_width_height(STDOUT_FILENO, &w_upg, &h_upg);
		printf("\033[1;%dr", h_upg - 1);

		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			char *url = xasprintf("%s/%s", p->repo_uri, p->filename);
			char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
			char *wget_cmd;
			char *new_args;

			update_progress(count - 1, total_upg, p->name);
			printf("\033[%d;1H", h_upg - 1);
			printf("%sGet:%d%s %s %s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, p->repo_uri, p->name, p->version, p->filename);

			wget_cmd = xasprintf("wget -q -O %s %s", tmp_deb, url);
			if (system(wget_cmd) == 0) {
				new_args = xasprintf("%s %s", dpkg_args, tmp_deb);
				free(dpkg_args);
				dpkg_args = new_args;
			}
			free(wget_cmd);
			free(url);
			curr = curr->link;
		}
		update_progress(total_upg, total_upg, "Done");

		if (*dpkg_args) {
			char *dpkg_cmd = xasprintf("dpkg%s -i %s", dpkg_force, dpkg_args);
			printf("\033[%d;1H", h_upg - 1);
			system(dpkg_cmd);
			free(dpkg_cmd);

			curr = install_queue;
			while (curr) {
				pkg_t *p = (pkg_t *)curr->data;
				char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
				unlink(tmp_deb);
				free(tmp_deb);
				curr = curr->link;
			}
		}
		printf("\033[r\033[%d;1H\n", h_upg);
		free(dpkg_args);
	} else if (strcmp(cmd, "list") == 0) {
		if (argc > 0 && strcmp(argv[0], "--upgradable") == 0) {
			FILE *f;
			load_all_packages();
			printf("Listing... Done\n");
			f = fopen_for_read("/var/lib/dpkg/status");
			if (f) {
				char *line;
				char *curr_pkg = NULL, *curr_ver = NULL;
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
		int res_count = 0;
		int search_idx;

		if (argc == 0) bb_show_usage();
		load_all_packages();

		/* Collect results */
		for (p = all_packages; p; p = p->next) {
			for (search_idx = 0; search_idx < argc; search_idx++) {
				if (strstr(p->name, argv[search_idx]) || (p->description && strstr(p->description, argv[search_idx]))) {
					results = xrealloc_vector(results, 3, res_count);
					results[res_count++] = p;
					break;
				}
			}
		}

		if (res_count > 0) {
			int j;
			bb_simple_info_msg("Sorting...");
			/* Simple bubble sort for search results */
			for (i = 0; i < res_count - 1; i++) {
				for (j = 0; j < res_count - i - 1; j++) {
					if (strcmp(results[j]->name, results[j+1]->name) > 0) {
						pkg_t *tmp = results[j];
						results[j] = results[j+1];
						results[j+1] = tmp;
					}
				}
			}

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
	} else {
		bb_show_usage();
	}

	return EXIT_SUCCESS;
}
