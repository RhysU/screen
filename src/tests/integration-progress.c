#define _GNU_SOURCE
#define _XOPEN_SOURCE 600

#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "macros.h"

#define WAIT_MS 5000
#define QUIET_MS 700

#define PROGRESS "\033]9;4;"

static char session[64];
static bool script_c;	/* script takes -c, as on Linux, NetBSD and OpenBSD */
static int idle[2];	/* script's stdin, held open so input never ends */
static char seen[65536];
static size_t seen_len;

/* Read the client's output until needle appears or ms pass. */
static bool read_until(int fd, const char *needle, int ms)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };

	while (poll(&pfd, 1, ms) > 0) {
		ssize_t n = read(fd, seen + seen_len, sizeof(seen) - 1 - seen_len);

		if (n <= 0)
			break;
		seen_len += n;
		seen[seen_len] = '\0';
		if (needle && strstr(seen, needle))
			return true;
	}
	return false;
}

static void forget(void)
{
	seen_len = 0;
	seen[0] = '\0';
}

static void expect(int fd, const char *needle)
{
	ASSERT(read_until(fd, needle, WAIT_MS));
	forget();
}

static void expect_quiet(int fd, const char *needle)
{
	read_until(fd, NULL, QUIET_MS);
	ASSERT(strstr(seen, needle) == NULL);
	forget();
}

/* Run ./screen -S session followed by args. */
static void screen_x(char *const args[])
{
	char *argv[16] = { "./screen", "-S", session };
	int status;
	pid_t pid;
	int i;

	for (i = 0; args[i]; i++)
		argv[i + 3] = args[i];
	argv[i + 3] = NULL;
	pid = fork();
	ASSERT(pid >= 0);
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	ASSERT(waitpid(pid, &status, 0) == pid);
	ASSERT(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void stuff(const char *win, const char *line)
{
	screen_x((char *[]){ "-p", (char *)win, "-X", "stuff", (char *)line, NULL });
}

/* Select win and expect needle as the first progress sent. */
static void select_win(int fd, const char *win, const char *needle)
{
	screen_x((char *[]){ "-X", "select", (char *)win, NULL });
	ASSERT(read_until(fd, needle, WAIT_MS));
	ASSERT(strstr(seen, PROGRESS) == strstr(seen, needle));
	forget();
}

/* Quit the session when an ASSERT aborts. */
static void quit_session(int sig)
{
	if (fork() == 0) {
		execl("./screen", "./screen", "-S", session, "-X", "quit", (char *)NULL);
		_exit(127);
	}
	signal(sig, SIG_DFL);
	raise(sig);
}

/*
 * Run cmd under script and return a pipe carrying the client's output.
 * FreeBSD and macOS take the command after the file instead of -c.
 */
static pid_t spawn_client(int *out, const char *cmd)
{
	int fds[2];
	pid_t pid;

	ASSERT(pipe(fds) == 0);
	forget();
	pid = fork();
	ASSERT(pid >= 0);
	if (pid == 0) {
		dup2(idle[0], 0);
		dup2(fds[1], 1);
		if (script_c)
			execlp("script", "script", "-c", cmd, "/dev/null", (char *)NULL);
		else
			execlp("script", "script", "/dev/null", "/bin/sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	*out = fds[0];
	return pid;
}

int main(void)
{
	char cmd[256];
	int out;
	int status;
	pid_t pid;

	if (system("command -v script >/dev/null 2>&1") != 0) {
		fprintf(stderr, "SKIP: script not available\n");
		return 77;
	}
	script_c = system("script -c true /dev/null </dev/null >/dev/null 2>&1") == 0;
	ASSERT(setenv("TERM", "xterm", 1) == 0);
	ASSERT(setenv("PS1", "$ ", 1) == 0);
	ASSERT(setenv("SHELL", "/bin/sh", 1) == 0);
	snprintf(session, sizeof(session), "progress-%ld", (long)getpid());
	ASSERT(pipe(idle) == 0);
	signal(SIGABRT, quit_session);
	snprintf(cmd, sizeof(cmd), "./screen -c /dev/null -S %s /bin/sh", session);
	pid = spawn_client(&out, cmd);
	expect(out, "$ ");

	/* Front window passes progress through. */
	stuff("0", "printf '\\033]9;4;1;50\\007'\n");
	expect(out, PROGRESS "1;50\a");

	/* A new window without progress clears the bar. */
	screen_x((char *[]){ "-X", "screen", NULL });
	expect(out, PROGRESS "0\a");

	/* Background progress waits for a switch back. */
	stuff("0", "printf '\\033]9;4;2;30\\007'\n");
	expect_quiet(out, PROGRESS);
	screen_x((char *[]){ "-X", "select", "0", NULL });
	expect(out, PROGRESS "2;30\a");

	/* Plain OSC 9 and bad input are dropped and the stored state kept. */
	stuff("0", "printf '"
	    "\\033]9;hello\\007"
	    "\\033]9;4;5;10\\007"
	    "\\033]9;4;-1;5\\007"
	    "\\033]9;4;;5\\007"
	    "\\033]9;4; 1;5\\007"
	    "\\033]09;4;1;5\\007"
	    "\\033]9x;4;1;5\\007"
	    "\\033]9;4;04;5\\007"
	    "\\033]9;4;1; 5\\007"
	    "\\033]9;4;1;+5\\007"
	    "\\033]9;4;1;-5\\007"
	    "\\033]9;4;1;101\\007"
	    "\\033]9;4;99999999999999999999;1\\007"
	    "\\033]9;4;1;-99999999999999999999\\007"
	    "\\033]9;4;1;5\\033\\033]2;x\\007"
	    "'\n");
	expect_quiet(out, "\033]9");
	screen_x((char *[]){ "-X", "select", "1", NULL });
	expect(out, PROGRESS "0\a");
	screen_x((char *[]){ "-X", "select", "0", NULL });
	expect(out, PROGRESS "2;30\a");

	/* State 0 clears a shown bar. */
	stuff("0", "printf '\\033]9;4;0;0\\007'\n");
	expect(out, PROGRESS "0\a");

	/* Percent takes leading zeros as in ParseNum(). */
	stuff("0", "printf '\\033]9;4;4;007\\007'\n");
	expect(out, PROGRESS "4;7\a");

	/* One bar follows its window there and back, twice. */
	select_win(out, "1", PROGRESS "0\a");
	select_win(out, "0", PROGRESS "4;7\a");
	select_win(out, "1", PROGRESS "0\a");
	select_win(out, "0", PROGRESS "4;7\a");

	/* Two bars swap with no clear between. */
	stuff("1", "printf '\\033]9;4;2;30\\007'\n");
	expect_quiet(out, PROGRESS);
	select_win(out, "1", PROGRESS "2;30\a");
	select_win(out, "0", PROGRESS "4;7\a");
	select_win(out, "1", PROGRESS "2;30\a");
	select_win(out, "0", PROGRESS "4;7\a");

	/* No bars send nothing. */
	stuff("0", "printf '\\033]9;4;0\\007'\n");
	expect(out, PROGRESS "0\a");
	stuff("1", "printf '\\033]9;4;0\\007'\n");
	expect_quiet(out, PROGRESS);
	screen_x((char *[]){ "-X", "select", "1", NULL });
	screen_x((char *[]){ "-X", "select", "0", NULL });
	screen_x((char *[]){ "-X", "select", "1", NULL });
	screen_x((char *[]){ "-X", "select", "0", NULL });
	expect_quiet(out, PROGRESS);

	/* ST terminates as sent. */
	stuff("0", "printf '\\033]9;4;1;7\\033\\\\'\n");
	expect(out, PROGRESS "1;7\033\\");

	/* Detach clears the bar. */
	screen_x((char *[]){ "-X", "detach", NULL });
	expect(out, PROGRESS "0\a");

	ASSERT(waitpid(pid, &status, 0) == pid);

	/* Reattach sends progress set while detached. */
	stuff("0", "printf '\\033]9;4;3\\007'\n");
	poll(NULL, 0, QUIET_MS);
	snprintf(cmd, sizeof(cmd), "./screen -r %s", session);
	close(out);
	pid = spawn_client(&out, cmd);
	expect(out, PROGRESS "3;0\a");
	screen_x((char *[]){ "-X", "detach", NULL });
	ASSERT(waitpid(pid, &status, 0) == pid);
	screen_x((char *[]){ "-X", "quit", NULL });
	return 0;
}
