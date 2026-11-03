/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * An NVIDIA GPU moved between nouveau and NVIDIA's own module, on the KMS
 * backend alone (DESIGN *Another kernel driver, with the window open*):
 * there the program is the machine's only display client, so nothing but
 * the renderer holds the GPU, which a window server cannot promise -- Xorg
 * keeps NVIDIA's open through its X driver, and wlroots opens every card.
 * Under one the other boot entry is the way.
 *
 * The move is tools/gpu-driver.sh's, as root: unloading a module wants
 * root, which the library never has. The account reaches it through
 * systemd's socket in front of it (GPU_DRIVER_SOCKET), as a member of the
 * socket's group, where each connection is a service of its own: a move
 * there outlives this process, and one killed halfway is answered by the
 * service's rescue, where a helper of this process's would die with it
 * and leave the GPU with no driver. Where there is no socket, or the
 * account is not yet in its group, through `sudo -n`, which a sudoers
 * rule allows and which fails at once where none does rather than asking
 * for a password; root without the socket runs the helper itself. Both
 * are installed by `bare_driver.py --setup`. FNL_GPU_DRIVER_SOCKET and
 * FNL_GPU_DRIVER_HELPER name others.
 *
 * What the list needs is sysfs's, which anyone may read, so the list is
 * read here and runs nothing. It is empty where the card the backend scans
 * out on is an NVIDIA GPU's: the backend holds that card for the window's
 * life, so nothing could let the GPU go, and a program with one NVIDIA
 * GPU and the panel on it would otherwise be offered a move every attempt
 * of which the helper refuses; with two, the move of the other is refused
 * anyway (app_gpu_drivers_start()).
 *
 * Every entry runs on a thread that may wait -- fnl_init(), the render
 * thread with nothing built, the move's own thread -- never on the pump's,
 * and reads of the backend's state only the card, which open() set.
 */

/* For pipe2(), which glibc declares only under it: a pipe made and then
 * marked close-on-exec is inherited by a child another thread forks in
 * between, and a helper whose output a stranger also holds open never
 * gives this one end of file. The file is Linux's already (sysfs, the
 * kernel's modules), so the wider feature set costs it nothing; it has
 * to be asked for ahead of app.h, whose own pin is the narrower one every
 * other file builds under. It also declares environ, in unistd.h. */
#define _GNU_SOURCE

#include "core/app.h"

#include "backend/kms/kms_driver.h"
#include "backend/kms/kms_state.h"

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define GPU_DRIVER_HELPER "/usr/local/sbin/fenestal-gpu-driver"
#define GPU_DRIVER_SOCKET "/run/fenestal-gpu-driver.sock"
#define GPU_DRIVER_GROUP  "fenestal-gpu"
#define PCI_DEVICES       "/sys/bus/pci/devices"
/* A move takes 0.5 s each way on a GTX 1050 Ti (1.2 s onto nouveau on a
 * GTX 960M). It is never cut short -- killed between the unload and the
 * load it would leave the GPU with no driver -- so past this it is said
 * once to be slow, and waited for to its end. */
#define HELPER_MS 20000
/* The connection said to have no service behind it, where sudo is tried. */
#define NO_SOCKET (-2)

/* The two drivers, in the order the helper names them. */
static const char *const DRIVERS[] = { "nouveau", "nvidia" };
#define DRIVER_COUNT (sizeof DRIVERS / sizeof DRIVERS[0])

static const char *env_or(const char *name, const char *fallback)
{
	const char *p = getenv(name);
	return p && p[0] ? p : fallback;
}

static const char *helper_path(void)
{
	return env_or("FNL_GPU_DRIVER_HELPER", GPU_DRIVER_HELPER);
}

static const char *socket_path(void)
{
	return env_or("FNL_GPU_DRIVER_SOCKET", GPU_DRIVER_SOCKET);
}

/* The helper's output, a line at a time and each logged as it comes, so
 * that a move that waits says what for while it waits: NVIDIA's unbind
 * names its holders every few seconds. The socket's `status: <n>` is its
 * last line, the exit status a connection has no other way to give. */
typedef struct {
	char   line[512];
	size_t n;
	int    status;
} Lines;

static void lines_end(Lines *l)
{
	l->line[l->n] = '\0';
	if (strncmp(l->line, "status: ", 8) == 0)
		l->status = atoi(l->line + 8);
	else if (l->n)
		fnl_log(strncmp(l->line, "held:", 5) == 0 ? FNL_LOG_WARN
												   : FNL_LOG_INFO,
				"GPU driver: %s", l->line);
	l->n = 0;
}

static void lines_feed(Lines *l, const char *p, size_t k)
{
	for (size_t i = 0; i < k; i++)
		if (p[i] == '\n')
			lines_end(l);
		else if (l->n + 1 < sizeof l->line)
			l->line[l->n++] = p[i];
}

/* `fd` read into `l` to its end, or to the status line where `to_status`.
 * Never given up on: past HELPER_MS said once to be slow. */
static void lines_read(int fd, Lines *l, bool to_status)
{
	const double deadline = now_seconds() + HELPER_MS / 1e3;
	bool said = false;
	while (!(to_status && l->status >= 0)) {
		int wait_ms = -1;
		if (!said) {
			const double left = deadline - now_seconds();
			wait_ms = left > 0.0 ? (int)(left * 1e3) + 1 : 0;
		}
		struct pollfd pf = { .fd = fd, .events = POLLIN };
		const int r = poll(&pf, 1, wait_ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r < 0)
			break;
		if (r == 0) {
			fnl_log(FNL_LOG_WARN, "GPU driver: the move has run %d s and "
					"is waited for", HELPER_MS / 1000);
			said = true;
			continue;
		}
		char buf[512];
		const ssize_t k = read(fd, buf, sizeof buf);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0)
			break;
		lines_feed(l, buf, (size_t)k);
	}
	if (l->n)
		lines_end(l);
}

/* The move asked of the socket's service: its status, -1 where the
 * service ended without one -- killed, which its rescue answers -- or
 * NO_SOCKET where nothing listens or this account may not connect, with
 * why in `why`. */
static int via_socket(const char *driver, const char *pci, char *why,
					  size_t why_n)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	const char *path = socket_path();
	if (strlen(path) >= sizeof sa.sun_path) {
		snprintf(why, why_n, "%s is too long a socket path", path);
		return NO_SOCKET;
	}
	memcpy(sa.sun_path, path, strlen(path) + 1);
	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		snprintf(why, why_n, "no socket (%s)", strerror(errno));
		return NO_SOCKET;
	}
	if (connect(fd, (const struct sockaddr *)&sa, sizeof sa) < 0) {
		const int e = errno;
		close(fd);
		if (e == EACCES)
			snprintf(why, why_n, "%s is group %s's, which this account "
					 "is not in yet (a new login)", path, GPU_DRIVER_GROUP);
		else
			snprintf(why, why_n, "no service at %s (%s)", path,
					 strerror(e));
		return NO_SOCKET;
	}
	/* A driver's name and an address, each bounded by its type -- the
	 * name checked against DRIVERS by gpu_driver_bind(). */
	char req[GPU_DRIVER_NAME_MAX + GPU_PCI_MAX + 1];
	const int len = snprintf(req, sizeof req, "%s %s\n", driver, pci);
	for (int sent = 0; sent < len;) {
		const ssize_t k = send(fd, req + sent, (size_t)(len - sent),
							   MSG_NOSIGNAL);
		if (k < 0 && errno == EINTR)
			continue;
		if (k <= 0) {
			close(fd);
			snprintf(why, why_n, "%s took no request (%s)", path,
					 strerror(errno));
			return NO_SOCKET;
		}
		sent += (int)k;
	}
	Lines l = { .status = -1 };
	lines_read(fd, &l, true);
	close(fd);
	return l.status;
}

/* The helper run with `driver` and `pci`, through sudo where `sudo`, its
 * stdout and stderr logged; its exit status, or -1 where it did not
 * start. Waited for to its end (HELPER_MS). */
static int via_helper(const char *driver, const char *pci, bool sudo)
{
	const char *argv[6];
	int n = 0;
	if (sudo) {
		argv[n++] = "sudo";
		argv[n++] = "-n";
	}
	argv[n++] = helper_path();
	argv[n++] = driver;
	argv[n++] = pci;
	argv[n] = NULL;

	int fds[2];
	if (pipe2(fds, O_CLOEXEC) < 0)
		return -1;
	posix_spawn_file_actions_t fa;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null",
									 O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&fa, fds[1], STDERR_FILENO);
	/* A process group of its own, so that what is sent to this process's
	 * -- a terminal's hangup or interrupt, a harness stopping the program
	 * -- is not also the helper's; the helper ignores them from its lock
	 * on, and this keeps them from reaching sudo too. A group is all
	 * that takes; a session (POSIX_SPAWN_SETSID) would also part the
	 * helper from the terminal, which nothing here asks of it. */
	posix_spawnattr_t at;
	posix_spawnattr_init(&at);
	posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
	posix_spawnattr_setpgroup(&at, 0);
	pid_t pid;
	/* posix_spawn's argv is char *const[] for a historical reason; the
	 * strings are not written. */
	const int rc = posix_spawnp(&pid, argv[0], &fa, &at,
								(char *const *)(void *)argv, environ);
	posix_spawnattr_destroy(&at);
	posix_spawn_file_actions_destroy(&fa);
	close(fds[1]);
	if (rc != 0) {
		close(fds[0]);
		return -1;
	}
	Lines l = { .status = -1 };
	lines_read(fds[0], &l, false);
	close(fds[0]);

	int status = 0;
	pid_t w;
	do
		w = waitpid(pid, &status, 0);
	while (w < 0 && errno == EINTR);
	/* ECHILD: a caller that reaps its own children has the status; a move
	 * is then judged by what is bound after (app_gpu_driver_make()). */
	if (w != pid)
		return 0;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* The first line of a small sysfs file into `out`, its newline cut. */
static bool read_line(const char *path, char *out, size_t size)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	const bool ok = fgets(out, (int)size, f) != NULL;
	fclose(f);
	if (ok)
		out[strcspn(out, "\n")] = '\0';
	return ok;
}

/* Which of DRIVERS this kernel has a module for: loaded, or listed in its
 * modules.dep -- nouveau as the kernel's own, NVIDIA's as its DKMS built
 * it for this kernel, or not -- by the module's name, the path's last part
 * up to its first dot. Read once a process: what a kernel has does not
 * change while it runs, and the list is made again after every move. */
static pthread_once_t have_once = PTHREAD_ONCE_INIT;
static bool           have[DRIVER_COUNT];

static void drivers_here(void)
{
	size_t left = DRIVER_COUNT;
	for (size_t i = 0; i < DRIVER_COUNT; i++) {
		char path[64];
		snprintf(path, sizeof path, "/sys/module/%s", DRIVERS[i]);
		have[i] = access(path, F_OK) == 0;
		if (have[i])
			left--;
	}
	struct utsname u;
	if (!left || uname(&u) != 0)
		return;
	char path[sizeof u.release + 32];
	snprintf(path, sizeof path, "/lib/modules/%s/modules.dep", u.release);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	/* A line longer than the buffer comes in pieces, and only the first
	 * piece names its module: the rest are skipped to its end. */
	char line[1024];
	bool start = true;
	while (left && fgets(line, sizeof line, f)) {
		const size_t n = strlen(line);
		const bool whole = n && line[n - 1] == '\n';
		char *colon = start ? strchr(line, ':') : NULL;
		start = whole;
		if (!colon)
			continue;
		*colon = '\0';
		const char *slash = strrchr(line, '/');
		const char *base = slash ? slash + 1 : line;
		const size_t len = strcspn(base, ".");
		for (size_t i = 0; i < DRIVER_COUNT; i++)
			if (!have[i] && len == strlen(DRIVERS[i]) &&
				memcmp(base, DRIVERS[i], len) == 0) {
				have[i] = true;
				left--;
			}
	}
	fclose(f);
}

/* Whether `name` is one of DRIVERS. */
static bool driver_known(const char *name)
{
	for (size_t i = 0; i < DRIVER_COUNT; i++)
		if (strcmp(DRIVERS[i], name) == 0)
			return true;
	return false;
}

int gpu_driver_list(const App *a, GpuDriver *out, int max)
{
	const struct BackendState *p = a->backend_state;
	if (access(socket_path(), F_OK) != 0 &&
		access(helper_path(), X_OK) != 0) {
		fnl_log(FNL_LOG_WARN, "FNL_GPU_DRIVER: neither %s nor %s, so no "
				"GPU moves to another kernel driver -- bare_driver.py "
				"--setup installs both", socket_path(), helper_path());
		return 0;
	}
	pthread_once(&have_once, drivers_here);
	DIR *d = opendir(PCI_DEVICES);
	if (!d)
		return 0;
	int n = 0;
	const struct dirent *e;
	while ((e = readdir(d)) && n < max) {
		const size_t len = strlen(e->d_name);
		if (e->d_name[0] == '.' || len >= GPU_PCI_MAX)
			continue;
		char path[300], word[32];
		/* An NVIDIA display device: vendor 0x10de, class 0x03. */
		snprintf(path, sizeof path, PCI_DEVICES "/%s/vendor", e->d_name);
		if (!read_line(path, word, sizeof word) || strcmp(word, "0x10de"))
			continue;
		snprintf(path, sizeof path, PCI_DEVICES "/%s/class", e->d_name);
		if (!read_line(path, word, sizeof word) ||
			strncmp(word, "0x03", 4) != 0)
			continue;
		/* The display's own card (see the top). */
		snprintf(path, sizeof path, PCI_DEVICES "/%s/drm/card%d", e->d_name,
				 p->card_index);
		if (access(path, F_OK) == 0) {
			fnl_log(FNL_LOG_INFO, "GPU driver: %s is the display's card, "
					"which this program holds, so no GPU here is moved",
					e->d_name);
			closedir(d);
			return 0;
		}
		GpuDriver g = { .bound = "none", .other = "" };
		memcpy(g.pci, e->d_name, len + 1);
		char link[300];
		snprintf(path, sizeof path, PCI_DEVICES "/%s/driver", e->d_name);
		const ssize_t k = readlink(path, link, sizeof link - 1);
		if (k > 0) {
			link[k] = '\0';
			const char *slash = strrchr(link, '/');
			snprintf(g.bound, sizeof g.bound, "%.*s",
					 (int)sizeof g.bound - 1, slash ? slash + 1 : link);
		}
		/* Moved from one of the two, or from none: a GPU on a third
		 * driver -- passed through to a machine, say -- is nobody's to
		 * move, and the helper would refuse it. */
		if (strcmp(g.bound, "none") == 0 || driver_known(g.bound))
			for (size_t i = 0; i < DRIVER_COUNT && !g.other[0]; i++)
				if (have[i] && strcmp(DRIVERS[i], g.bound) != 0)
					snprintf(g.other, sizeof g.other, "%s", DRIVERS[i]);
		if (g.other[0]) {
			snprintf(path, sizeof path, "/sys/module/%s", g.other);
			g.other_loaded = access(path, F_OK) == 0;
		}
		/* In address order, as the helper's list is: which GPU a move
		 * without an address takes is its first. */
		int at = n++;
		while (at > 0 && strcmp(out[at - 1].pci, g.pci) > 0) {
			out[at] = out[at - 1];
			at--;
		}
		out[at] = g;
	}
	closedir(d);
	return n;
}

bool gpu_driver_bind(const App *a, const GpuDriver *g, const char *driver)
{
	(void)a;
	/* FNL_GPU_DRIVER's own word, where the GPU is on no driver: the
	 * helper's check, made here first so that the request is bounded. */
	if (!driver_known(driver)) {
		fnl_log(FNL_LOG_WARN, "GPU driver: no driver %s here: one of %s "
				"and %s", driver, DRIVERS[0], DRIVERS[1]);
		return false;
	}
	fnl_log(FNL_LOG_INFO, "GPU driver: moving %s from %s to %s", g->pci,
			g->bound, driver);
	/* The socket first for root as well: a move there outlives this
	 * process, where a helper of its own dies with its session. */
	char why[256];
	const bool root = geteuid() == 0;
	int rc = via_socket(driver, g->pci, why, sizeof why);
	if (rc == NO_SOCKET) {
		fnl_log(FNL_LOG_INFO, "GPU driver: %s, so %s; a move cut off there "
				"leaves the GPU with no driver", why,
				root ? "by the helper itself" : "through sudo");
		rc = via_helper(driver, g->pci, !root);
		if (rc < 0) {
			fnl_log(FNL_LOG_WARN, "GPU driver: %s did not run%s",
					helper_path(), root ? "" : " through sudo -n, which "
					"wants the rule bare_driver.py --setup writes");
			return false;
		}
	}
	if (rc == 0)
		return true;
	if (rc == 2)
		fnl_log(FNL_LOG_WARN, "GPU driver: %s is held open, and stays on "
				"%s", g->pci, g->bound);
	else if (rc < 0)
		fnl_log(FNL_LOG_WARN, "GPU driver: the move to %s ended with no "
				"answer -- the service killed, whose rescue binds a "
				"driver", driver);
	else
		fnl_log(FNL_LOG_WARN, "GPU driver: the move to %s failed "
				"(status %d)", driver, rc);
	return false;
}

/* NVIDIA's libraries mapped now, by file name, after what `what` says:
 * the descriptor names the driver held and not who opened it. A library
 * still mapped with nothing built is a caller's or one the dynamic linker
 * kept past its close (egl_dyn.c); none mapped is a descriptor its
 * library left open as it was unloaded. Each mapping of a library is a
 * line of its own, consecutive, so the last name alone is compared. */
static void nvidia_mapped(char *what, size_t size)
{
	FILE *f = fopen("/proc/self/maps", "r");
	if (!f)
		return;
	size_t used = strlen(what);
	char line[700];
	char last[256] = "";
	bool any = false;
	while (fgets(line, sizeof line, f) && used + 1 < size) {
		line[strcspn(line, "\n")] = '\0';
		const char *path = strchr(line, '/');
		if (!path)
			continue;
		const char *base = strrchr(path, '/') + 1;
		if (!strstr(base, "nvidia") || strcmp(base, last) == 0)
			continue;
		snprintf(last, sizeof last, "%s", base);
		const int k = snprintf(what + used, size - used, "%s%s",
							   any ? ", " : " (mapped: ", base);
		if (k < 0)
			break;
		used += (size_t)k < size - used ? (size_t)k : size - used - 1;
		any = true;
	}
	fclose(f);
	snprintf(what + used, size - used, "%s",
			 any ? ")" : " (no library of NVIDIA's mapped)");
}

bool gpu_driver_held(const App *a, const GpuDriver *g, bool every_nvidia,
					 char *what, size_t size)
{
	(void)a;
	/* The GPU's own nodes, by name: card1, renderD129. */
	char nodes[8][300];
	int n_nodes = 0;
	char path[300];
	snprintf(path, sizeof path, PCI_DEVICES "/%s/drm", g->pci);
	DIR *d = opendir(path);
	if (d) {
		const struct dirent *e;
		while ((e = readdir(d)) && n_nodes < 8)
			if (strncmp(e->d_name, "card", 4) == 0 ||
				strncmp(e->d_name, "renderD", 7) == 0)
				snprintf(nodes[n_nodes++], sizeof nodes[0], "/dev/dri/%s",
						 e->d_name);
		closedir(d);
	}
	/* NVIDIA's module counts every /dev/nvidia* as a hold on its GPUs,
	 * nouveau only the GPU's own nodes (the helper's holders()). Any is
	 * NVIDIA's userspace left loaded, where every one is asked for. */
	const bool nvidia = every_nvidia || strcmp(g->bound, "nvidia") == 0;
	d = opendir("/proc/self/fd");
	if (!d)
		return false;
	bool held = false;
	const struct dirent *e;
	while (!held && (e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		char link[300];
		snprintf(path, sizeof path, "/proc/self/fd/%s", e->d_name);
		const ssize_t k = readlink(path, link, sizeof link - 1);
		if (k <= 0)
			continue;
		link[k] = '\0';
		held = nvidia && strncmp(link, "/dev/nvidia", 11) == 0;
		for (int i = 0; i < n_nodes && !held; i++)
			held = strcmp(link, nodes[i]) == 0;
		if (held)
			snprintf(what, size, "%s", link);
	}
	closedir(d);
	if (held && strncmp(what, "/dev/nvidia", 11) == 0)
		nvidia_mapped(what, size);
	return held;
}
