/* vi: set sw=4 ts=4: */
/*
 * micro lpd
 *
 * Copyright (C) 2008 by Vladimir Dronnikov <dronnikov@gmail.com>
 *
 * Licensed under GPLv2, see file LICENSE in this tarball for details.
 */

/*
 * A typical usage of BB lpd looks as follows:
 * # tcpsvd -E 0 515 lpd SPOOLDIR [HELPER-PROG [ARGS...]]
 *
 * This means a network listener is started on port 515 (default for LP protocol).
 * When a client connection is made (via lpr) lpd first change its working directory to SPOOLDIR.
 *
 * SPOOLDIR is the spool directory which contains printing queues
 * and should have the following structure:
 *
 * SPOOLDIR/
 * 	<queue1>
 * 	...
 * 	<queueN>
 *
 * <queueX> can be of two types:
 * 	A. a printer character device or an ordinary file a link to such;
 * 	B. a directory.
 *
 * In case A lpd just dumps the data it receives from client (lpr) to the
 * end of queue file/device. This is non-spooling mode.
 *
 * In case B lpd enters spooling mode. It reliably saves client data along with control info
 * in two unique files under the queue directory. These files are named dfAXXXHHHH and cfAXXXHHHH,
 * where XXX is the job number and HHHH is the client hostname. Unless a printing helper application
 * is specified lpd is done at this point.
 *
 * NB: file names are produced by peer! They actually may be anything at all!
 * lpd only sanitizes them (by removing most non-alphanumerics).
 *
 * If HELPER-PROG (with optional arguments) is specified then lpd continues to process client data:
 * 	1. it reads and parses control file (cfA...). The parse process results in setting environment
 * 	variables whose values were passed in control file; when parsing is complete, lpd deletes
 * 	control file.
 * 	2. it spawns specified helper application. It is then the helper application who is responsible
 * 	for both actual printing and deleting processed data file.
 *
 * A good lpr passes control files which when parsed provide the following variables:
 * $H = host which issues the job
 * $P = user who prints
 * $C = class of printing (what is printed on banner page)
 * $J = the name of the job
 * $L = print banner page
 * $M = the user to whom a mail should be sent if a problem occurs
 * $l = name of datafile ("dfAxxx") - file whose content are to be printed
 *
 * lpd also provides $DATAFILE environment variable - the ACTUAL name
 * of the datafile under which it was saved.
 * $l is not reliable (you are at mercy of remote peer), DON'T USE IT.
 *
 * Thus, a typical helper can be something like this:
 * #!/bin/sh
 * cat "$l" >/dev/lp0
 * mv -f "$l" save/
 */

#include "libbb.h"

// strip argument of bad chars
static char *sane(char *str)
{
	char *s = str;
	char *p = s;
	while (*s) {
		if (isalnum(*s) || '-' == *s || '_' == *s) {
			*p++ = *s;
		}
		s++;
	}
	*p = '\0';
	return str;
}

// we can use leaky setenv since we are about to exec or exit
static void exec_helper(char **filenames, char **argv) ATTRIBUTE_NORETURN;
static void exec_helper(char **filenames, char **argv)
{
	char *p, *q;
	char var[2];

	// read and delete ctrlfile
	q = xmalloc_open_read_close(filenames[0], NULL);
	unlink(filenames[0]);
	// provide datafile name
	xsetenv("DATAFILE", filenames[1]);
	// parse control file by "\n"
	while ((p = strchr(q, '\n')) != NULL
	 && isalpha(*q)
	) {
		*p++ = '\0';
		// here q is a line of <SYM><VALUE>
		// let us set environment string <SYM>=<VALUE>
		var[0] = *q++;
		var[1] = '\0';
		xsetenv(var, q);
		// next line, plz!
		q = p;
	}
	// we are the helper, we wanna be silent.
	// this call reopens stdio fds to "/dev/null"
	// (no daemonization is done)
	bb_daemonize_or_rexec(DAEMON_DEVNULL_STDIO | DAEMON_ONLY_SANITIZE, NULL);
	BB_EXECVP(*argv, argv);
	exit(0);
}

static char *xmalloc_read_stdin(void)
{
	// SECURITY:
	size_t max = 4 * 1024; // more than enough for commands!
	return xmalloc_reads(STDIN_FILENO, NULL, &max);
}

int lpd_main(int argc, char *argv[]) MAIN_EXTERNALLY_VISIBLE;
int lpd_main(int argc ATTRIBUTE_UNUSED, char *argv[])
{
	int spooling = spooling; // for compiler
	int seen;
	char *s, *queue;
	char *filenames[2];

	// goto spool directory
	if (*++argv)
		xchdir(*argv++);

	// error messages of xfuncs will be sent over network
	xdup2(STDOUT_FILENO, STDERR_FILENO);

	filenames[0] = NULL; // ctrlfile name
	filenames[1] = NULL; // datafile name

	// read command
	s = queue = xmalloc_read_stdin();
	// we understand only "receive job" command
	if (2 != *queue) {
 unsupported_cmd:
		printf("Command %02x %s\n",
			(unsigned char)s[0], "is not supported");
		goto err_exit;
	}

	// parse command: "2 | QUEUE_NAME | '\n'"
	queue++;
	// protect against "/../" attacks
	// *strchrnul(queue, '\n') = '\0'; - redundant, sane() will do
	if (!*sane(queue))
		return EXIT_FAILURE;

	// queue is a directory -> chdir to it and enter spooling mode
	spooling = chdir(queue) + 1; // 0: cannot chdir, 1: done
	seen = 0;
	// we don't free(queue), we might need it later

	while (1) {
		char *fname;
		int fd;
		// int is easier than ssize_t: can use xatoi_u,
		// and can correctly display error returns (-1)
		int expected_len, real_len;

		// signal OK
		safe_write(STDOUT_FILENO, "", 1);

		// get subcommand
		// valid s must be of form: "SUBCMD | LEN | space | FNAME"
		// N.B. we bail out on any error
		s = xmalloc_read_stdin();
		if (!s) { // (probably) EOF
			if (spooling /* && 6 != spooling - always true */) {
				// we didn't see both ctrlfile & datafile!
				goto err_exit;
			}
			// one of only two non-error exits
			return EXIT_SUCCESS;
		}

		// validate input.
		// we understand only "control file" or "data file" cmds
		if (2 != s[0] && 3 != s[0])
			goto unsupported_cmd;
		if (seen & (s[0] - 1)) {
			printf("Duplicated subcommand\n");
			goto err_exit;
		}
		seen &= (s[0] - 1); // bit 1: ctrlfile; bit 2: datafile
		// get filename
		*strchrnul(s, '\n') = '\0';
		fname = strchr(s, ' ');
		if (!fname) {
// bad_fname:
			printf("No or bad filename\n");
			goto err_exit;
		}
		*fname++ = '\0';
//		// s[0]==2: ctrlfile, must start with 'c'
//		// s[0]==3: datafile, must start with 'd'
//		if (fname[0] != s[0] + ('c'-2))
//			goto bad_fname;
		// get length
		expected_len = bb_strtou(s + 1, NULL, 10);
		if (errno || expected_len < 0) {
			printf("Bad length\n");
			goto err_exit;
		}
		if (2 == s[0] && expected_len > 16 * 1024) {
			// SECURITY:
			// ctrlfile can't be big (we want to read it back later!)
			printf("File is too big\n");
			goto err_exit;
		}

		// open the file
		if (spooling) {
			// spooling mode: dump both files
			// job in flight has mode 0200 "only writable"
			sane(fname);
			fd = open3_or_warn(fname, O_CREAT | O_WRONLY | O_TRUNC | O_EXCL, 0200);
			if (fd < 0)
				goto err_exit;
			filenames[s[0] - 2] = xstrdup(fname);
		} else {
			// non-spooling mode:
			// 2: control file (ignoring), 3: data file
			fd = -1;
			if (3 == s[0])
				fd = xopen(queue, O_RDWR | O_APPEND);
		}

		// copy the file
		real_len = bb_copyfd_size(STDIN_FILENO, fd, expected_len);
		if (real_len != expected_len) {
			printf("Expected %d but got %d bytes\n",
				expected_len, real_len);
			goto err_exit;
		}
		// get ACK and see whether it is NUL (ok)
		if (safe_read(STDIN_FILENO, s, 1) != 1 || s[0] != 0) {
			// don't send error msg to peer - it obviously
			// don't follow the protocol, so probably
			// it can't understand us either
			goto err_exit;
		}

		if (spooling) {
			// chmod completely downloaded file as "readable+writable"
			fchmod(fd, 0600);
			// accumulate dump state
			// N.B. after all files are dumped spooling should be 1+2+3==6
			spooling += s[0];
		}
		free(s);
		close(fd); // NB: can do close(-1). Who cares?

		// spawn spool helper and exit if all files are dumped
		if (6 == spooling && *argv) {
			// signal OK
			safe_write(STDOUT_FILENO, "", 1);
			// does not return (exits 0)
			exec_helper(filenames, argv);
		}
	} // while (1)

 err_exit:
	// don't keep corrupted files
	if (spooling) {
		if (filenames[0])
			unlink(filenames[0]);
		if (filenames[1])
			unlink(filenames[1]);
	}
	return EXIT_FAILURE;
}