// SPDX-License-Identifier: GPL-2.0
/*
 * Stream /dev/kmsg to a file, one fdatasync per record.
 *
 * For failures that reset the board rather than panicking it. Nothing buffered
 * survives such a reset: journald has not flushed, and on this board even the
 * ramoops console area comes back empty, so the last seconds -- the
 * interesting ones -- are always missing. Writing each record through to
 * storage before reading the next one costs throughput but means the log ends
 * exactly where the machine died.
 *
 * Build and run on the device:
 *
 *     gcc -O2 -o kmsg_sync_log kmsg_sync_log.c
 *     sudo ./kmsg_sync_log /var/log/kcrash.log &
 *
 * /dev/kmsg returns one record per read() and never partial ones, so no
 * reassembly is needed. Records are dropped rather than blocking if the reader
 * falls behind; that shows up as a jump in the sequence numbers each line
 * carries, which is preferable to stalling the machine under test.
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop;

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

int main(int argc, char **argv)
{
	char buf[8192];
	int kmsg, out;
	ssize_t n;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <logfile>\n", argv[0]);
		return 1;
	}

	kmsg = open("/dev/kmsg", O_RDONLY);
	if (kmsg < 0) {
		perror("open /dev/kmsg");
		return 1;
	}

	/* Start at the oldest record still held, so boot context is included. */
	lseek(kmsg, 0, SEEK_SET);

	out = open(argv[1], O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (out < 0) {
		perror("open logfile");
		return 1;
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	while (!stop) {
		n = read(kmsg, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EPIPE)	/* fell behind, records lost */
				continue;
			if (errno == EINTR)
				continue;
			perror("read");
			break;
		}

		if (write(out, buf, n) != n) {
			perror("write");
			break;
		}

		/*
		 * The whole point: without this the record sits in page cache
		 * and is lost with everything else when the board resets.
		 */
		if (fdatasync(out) < 0) {
			perror("fdatasync");
			break;
		}
	}

	close(out);
	close(kmsg);
	return 0;
}
