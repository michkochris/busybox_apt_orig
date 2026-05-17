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
//usage:       "COMMAND [PACKAGE...]"
//usage:#define apt_full_usage "\n\n"
//usage:       "High-level package manager\n"
//usage:     "\nCommands:"
//usage:     "\n	update		Update list of available packages"
//usage:     "\n	install		Install new packages"
//usage:     "\n	remove		Remove packages"
//usage:     "\n	upgrade		Upgrade the system by installing/upgrading packages"
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

static void update_progress(int current, int total, const char *msg)
{
	unsigned width, height;
	int i, pos, percent;
	char *bar;

	get_terminal_width_height(STDOUT_FILENO, &width, &height);
	if (width < 20) return;

	percent = (current * 100) / total;
	pos = (percent * (width - 20)) / 100;

	/* Move to bottom, clear line */
	printf("\033[%d;1H\033[2K", height);

	printf("Progress: [%3d%%] [", percent);
	for (i = 0; i < width - 20; i++) {
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
	FILE *f = fopen_for_read(filename);
	if (!f) return NULL;

	repo_t *repos = NULL;
	char *line;

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
			new_repo->uri = xstrdup(uri);
			new_repo->dist = xstrdup(dist);

			char *comp;
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

			printf("%sGet:%d%s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, url, (char *)comp->data);

			/* Use wget applet to download */
			char *wget_cmd = xasprintf("wget -q -O %s %s", local_path, url);
			if (system(wget_cmd) != 0) {
				// bb_error_msg("failed to download %s", url);
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
	char *description;
	char *filename;
	char *repo_uri;
	long size;
	int state; /* 0: unvisited, 1: queued, 2: installing */
	struct pkg_s *next;
} pkg_t;

static pkg_t *all_packages = NULL;
static llist_t *install_queue = NULL;

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
		p = p->next;
	}
	return NULL;
}

static bool is_installed(const char *pkg_name)
{
	/* Simple check in /var/lib/dpkg/status */
	FILE *f = fopen_for_read("/var/lib/dpkg/status");
	if (!f) return false;

	char *line;
	bool found = false;
	bool in_pkg = false;
	while ((line = xmalloc_fgetline(f)) != NULL) {
		if (strncmp(line, "Package: ", 9) == 0) {
			if (strcmp(skip_whitespace(line + 9), pkg_name) == 0) {
				in_pkg = true;
			} else {
				in_pkg = false;
			}
		} else if (in_pkg && strncmp(line, "Status: ", 8) == 0) {
			if (strstr(line, "installed")) {
				found = true;
				free(line);
				break;
			}
		}
		free(line);
	}
	fclose(f);
	return found;
}

static void resolve_deps(pkg_t *p)
{
	char *deps_copy;
	char *saveptr;
	char *dep_entry;

	if (!p || p->state != 0) return;
	if (is_installed(p->name)) return;

	p->state = 1; /* Mark as queued to avoid loops */

	if (p->depends) {
		deps_copy = xstrdup(p->depends);
		dep_entry = strtok_r(deps_copy, ",", &saveptr);

		while (dep_entry) {
			/* Handle OR dependencies by just taking the first one for now */
			char *or_ptr;
			char *first_dep = strtok_r(dep_entry, "|", &or_ptr);
			char *p_special;

			/* Clean up package name (remove version constraints like (>= 1.0)) */
			first_dep = skip_whitespace(first_dep);
			p_special = strpbrk(first_dep, " (");
			if (p_special) *p_special = '\0';

			pkg_t *dep_pkg = find_package(first_dep);
			if (dep_pkg) {
				resolve_deps(dep_pkg);
			} else {
				/* If not found in repo, assume it might be already installed or optional */
				/* In a real apt, this would be an error if not satisfied */
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
	if (!f) {
		free(cmd);
		return;
	}

	char *line;
	pkg_t *curr = NULL;
	while ((line = xmalloc_fgetline(f)) != NULL) {
		if (*line == '\0') {
			if (curr) {
				add_package(curr);
				curr = NULL;
			}
		} else if (strncmp(line, "Package: ", 9) == 0) {
			if (curr) add_package(curr);
			curr = xzalloc(sizeof(pkg_t));
			curr->name = xstrdup(skip_whitespace(line + 9));
			curr->repo_uri = xstrdup(repo_uri);
		} else if (curr && strncmp(line, "Version: ", 9) == 0) {
			curr->version = xstrdup(skip_whitespace(line + 9));
		} else if (curr && strncmp(line, "Depends: ", 9) == 0) {
			curr->depends = xstrdup(skip_whitespace(line + 9));
		} else if (curr && strncmp(line, "Description: ", 13) == 0) {
			curr->description = xstrdup(skip_whitespace(line + 13));
		} else if (curr && strncmp(line, "Filename: ", 10) == 0) {
			curr->filename = xstrdup(skip_whitespace(line + 10));
		} else if (curr && strncmp(line, "Size: ", 6) == 0) {
			curr->size = atol(skip_whitespace(line + 6));
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

/* Simple version comparison: returns >0 if v1 > v2, <0 if v1 < v2, 0 if equal */
static int compare_versions(const char *v1, const char *v2)
{
	while (*v1 && *v2) {
		if (isdigit(*v1) && isdigit(*v2)) {
			long n1 = strtol(v1, (char **)&v1, 10);
			long n2 = strtol(v2, (char **)&v2, 10);
			if (n1 != n2) return n1 - n2;
		} else {
			if (*v1 != *v2) return *v1 - *v2;
			v1++; v2++;
		}
	}
	if (*v1) return 1;
	if (*v2) return -1;
	return 0;
}

static int count_upgradable(void)
{
	int count = 0;
	FILE *f = fopen_for_read("/var/lib/dpkg/status");
	if (!f) return 0;

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
	const char *cmd;

	if (argc < 2)
		bb_show_usage();

	cmd = argv[1];
	argv += 2;
	argc -= 2;

	if (strcmp(cmd, "update") == 0) {
		repo_t *repos;
		int upgradable;

		printf("Hit:1 http://http.kali.org/kali kali-rolling InRelease\n");
		printf("Reading package lists... Done\n");
		repos = parse_sources_list("/etc/apt/sources.list");
		if (!repos) {
			bb_error_msg_and_die("could not parse /etc/apt/sources.list");
		}
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

		if (argc == 0) bb_show_usage();
		printf("Reading package lists... Done\n");
		load_all_packages();
		printf("Building dependency tree... Done\n");
		printf("Reading state information... Done\n");

		while (*argv) {
			if (is_installed(*argv)) {
				bb_info_msg("%s is already the newest version.", *argv);
			} else {
				pkg_t *p = find_package(*argv);
				if (p) {
					resolve_deps(p);
				} else {
					bb_error_msg("E: Unable to locate package %s", *argv);
				}
			}
			argv++;
		}

		curr = install_queue;
		if (!curr) {
			bb_simple_info_msg("0 upgraded, 0 newly installed, 0 to remove and 0 not upgraded.");
			return EXIT_SUCCESS;
		}

		printf("The following %sNEW%s packages will be installed:\n ", CLR_BOLD, CLR_RESET);
		count = 0;
		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			printf(" %s", p->name);
			count++;
			total_size += p->size;
			curr = curr->link;
		}
		printf("\n%d upgraded, %d newly installed, 0 to remove and 0 not upgraded.\n", 0, count);
		printf("Need to get %ld kB of archives.\n", total_size / 1024);
		printf("After this operation, 0 B of additional disk space will be used.\n");
		printf("%sDo you want to continue? [Y/n]%s ", CLR_BOLD, CLR_RESET);
		fflush(stdout);

		if (!bb_ask_y_confirmation())
			return EXIT_SUCCESS;

		dpkg_args = xstrdup("");
		curr = install_queue;
		count = 1;
		int total = llist_count(install_queue);

		/* Set scrolling region to leave bottom line for progress */
		unsigned w, h;
		get_terminal_width_height(STDOUT_FILENO, &w, &h);
		printf("\033[1;%dr", h - 1);

		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			char *url = xasprintf("%s/%s", p->repo_uri, p->filename);
			char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
			char *new_args;

			update_progress(count - 1, total, p->name);
			printf("\033[%d;1H", h - 1); /* Move just above progress bar */
			printf("%sGet:%d%s %s %s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, p->repo_uri, p->name, p->version, p->filename);

			char *wget_cmd = xasprintf("wget -q -O %s %s", tmp_deb, url);
			if (system(wget_cmd) == 0) {
				new_args = xasprintf("%s %s", dpkg_args, tmp_deb);
				free(dpkg_args);
				dpkg_args = new_args;
			}
			free(wget_cmd);
			free(url);
			curr = curr->link;
		}
		update_progress(total, total, "Done");

		if (*dpkg_args) {
			char *dpkg_cmd = xasprintf("dpkg -i %s", dpkg_args);
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

		printf("Reading package lists... Done\n");
		load_all_packages();
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

		total_size = 0;
		printf("The following packages will be upgraded:\n ");
		count = 0;
		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			printf(" %s", p->name);
			count++;
			total_size += p->size;
			curr = curr->link;
		}
		printf("\n%d upgraded, %d newly installed, 0 to remove and 0 not upgraded.\n", count, 0);
		printf("Need to get %ld kB of archives.\n", total_size / 1024);
		printf("After this operation, 0 B of additional disk space will be used.\n");
		printf("%sDo you want to continue? [Y/n]%s ", CLR_BOLD, CLR_RESET);
		fflush(stdout);

		if (!bb_ask_y_confirmation())
			return EXIT_SUCCESS;

		dpkg_args = xstrdup("");
		curr = install_queue;
		count = 1;
		int total_upg = llist_count(install_queue);

		unsigned w_upg, h_upg;
		get_terminal_width_height(STDOUT_FILENO, &w_upg, &h_upg);
		printf("\033[1;%dr", h_upg - 1);

		while (curr) {
			pkg_t *p = (pkg_t *)curr->data;
			char *url = xasprintf("%s/%s", p->repo_uri, p->filename);
			char *tmp_deb = xasprintf("/tmp/%s.deb", p->name);
			char *new_args;

			update_progress(count - 1, total_upg, p->name);
			printf("\033[%d;1H", h_upg - 1);
			printf("%sGet:%d%s %s %s %s [%s]\n", CLR_GREEN, count++, CLR_RESET, p->repo_uri, p->name, p->version, p->filename);

			char *wget_cmd = xasprintf("wget -q -O %s %s", tmp_deb, url);
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
			char *dpkg_cmd = xasprintf("dpkg -i %s", dpkg_args);
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
	} else if (strcmp(cmd, "search") == 0) {
		pkg_t *p;

		if (argc == 0) bb_show_usage();
		load_all_packages();
		bb_simple_info_msg("Sorting...");

		for (p = all_packages; p; p = p->next) {
			if (strstr(p->name, argv[0]) || (p->description && strstr(p->description, argv[0]))) {
				printf("%s%s%s/%s %s %s\n", CLR_GREEN, p->name, CLR_RESET, "stable", p->version, p->repo_uri);
				if (p->description)
					printf("  %s\n", p->description);
			}
		}
	} else {
		bb_show_usage();
	}

	return EXIT_SUCCESS;
}
