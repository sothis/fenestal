/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2026 Janos Sonntag
 *
 * What a tool here needs to speak the MCP port and to start the program it
 * speaks to: the JSON-RPC framing over a socket or a command's stdio, the
 * little JSON reading that answers of a known shape want, and a program
 * started, logged and waited for. round (tools/drive/round.c) drives a
 * window with it; pace measures one.
 *
 * Nothing here is the library's: this is the client side of
 * src/core/mcp.c, written for the tools that drive a program from outside
 * it, and it links nothing.
 */
#ifndef FNL_DRIVE_MCP_CLIENT_H
#define FNL_DRIVE_MCP_CLIENT_H

#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>

/* The environment a program is handed: glibc's, declared here because
 * _POSIX_C_SOURCE does not name it. */
extern char **environ;

/* The longest path a port or a program's log can take here. */
#define SOCK_MAX 512

/* A growable string. */
typedef struct {
	char  *p;
	size_t n, cap;
} Str;

typedef struct {
	/* One descriptor for a socket, two for a command's pipes. */
	int   rfd, wfd;
	pid_t cmd;      /* the command, where the port is one; 0 for a socket */
	long  next_id;
	Str   buf;      /* the line being read */
	Str   text;     /* the last reply's first text item */
	Str   image;    /* ... and its image item, base64 */
	bool  err;
} Port;

typedef struct {
	pid_t pid;
	char  sock[512];  /* the first window's port */
	char  base[512];  /* ... and the prefix the ones after it add to */
	char  log_path[512];
	Str   lines;      /* its stderr, whole */
	bool  ended;
	int   rc;
} Prog;

/* The round's options, which the client's waits read: how long a reply may
 * take, the command a port is reached by, and the machine a program is
 * started on. */
extern int         g_timeout_s;
extern bool        g_quiet;
extern const char *g_command;
extern const char *g_ssh;
extern const char *g_remote_dir;

double now_s(void);
void   sleep_s(double s);
void   str_add(Str *s, const char *p, size_t n);
void   str_adds(Str *s, const char *p);
void   str_addc(Str *s, char c);
void   str_addf(Str *s, const char *fmt, ...);
void   str_free(Str *s);
const char *json_unescape(const char *p, Str *out);
bool json_str_at(const char *obj, const char *key, Str *out);
double json_num_at(const char *obj, const char *key, double dflt);
bool json_bool_at(const char *obj, const char *key, bool dflt);
bool rpc(Port *p, const char *method, const char *params, Str *reply);
const char *mcp_call(Port *p, const char *tool, const char *args_json);
const char *ask(Port *p, const char *tool, const char *fmt, ...);
int  words(char *buf, char **out, int max);
int  whole(const char *word, int dflt);
void dbg(const char *fmt, ...);
bool sh_run(const char *cmd, Str *out, int *status);
bool remote_gone(const char *host, long pid);
void port_spawn(Port *p, char *const argv[]);
void port_close(Port *p);
void lines_read(Prog *pr);
const char *socket_dir(void);
void prog_sockets(Prog *pr);
bool prog_wait_socket(Prog *pr, int n, double seconds);
void prog_start(Prog *pr, char *const argv[], char *const envp[],
				const char *log_path);
long remote_start(const char *host, const char *dir, const char *backend,
				  const char *renderer, const char *variant, char *log_out,
				  size_t log_n);
void remote_log(const char *host, const char *remote, const char *local);

#endif /* FNL_DRIVE_MCP_CLIENT_H */
