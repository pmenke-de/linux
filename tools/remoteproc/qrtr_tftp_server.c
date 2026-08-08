// SPDX-License-Identifier: GPL-2.0
/*
 * qrtr_tftp_server - remote filesystem (RFS) service for the SDX55 modem
 *
 * The SDX55 mounts part of its filesystem over TFTP carried on QRTR. Its own
 * F3 log on pmOS shows it failing at the very first step, forever:
 *
 *   ERR :[tftp_socket_ipcr_modem.c, 220] could not resolve remote host
 *   ERR :[tftp_protocol.c, 870] Timeout while WRQ
 *   DBG :[tftp_connection.c, 359] conn opened sd = 37055, name = [sid = 4096, iid = 3]
 *   INF :[tftp_client.c, 1289] TFTP_STAT : /readonly/firmware/image/modem_pr/so/534_0_0.mbn
 *   INF :[tftp_client.c, 1289] TFTP_STAT : /readwrite/mcfg.tmp
 *
 * i.e. it looks up QRTR service 4096 instance 3, finds nobody, and every read
 * and write times out having moved zero bytes - including /readwrite/mcfg.tmp,
 * an mcfg working file. Android runs /vendor/bin/tftp_server for this; pmOS
 * ran nothing, which is why the modem's carrier configuration never completes.
 *
 * Android's server maps each QRTR *instance* to a filesystem root, e.g.
 *   /vendor/rfs/mdm/mpss/{readonly,readwrite,shared,hlos,ramdumps}
 * with readonly/firmware backed by the modem partition and the rest writable.
 * The modem sends absolute paths like "/readonly/firmware/..." which are
 * resolved beneath that root.
 *
 * Build on the device (see tools/remoteproc/Makefile).
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/qrtr.h>

/* QRTR control */
#ifndef QRTR_PORT_CTRL
#define QRTR_PORT_CTRL		0xfffffffeu
#endif
/* struct qrtr_ctrl_pkt and QRTR_TYPE_* come from <linux/qrtr.h>. */
#ifndef QRTR_TYPE_NEW_SERVER
#define QRTR_TYPE_NEW_SERVER	4
#endif

/* TFTP opcodes (RFC 1350 + RFC 2347 options) */
#define TFTP_RRQ	1
#define TFTP_WRQ	2
#define TFTP_DATA	3
#define TFTP_ACK	4
#define TFTP_ERROR	5
#define TFTP_OACK	6

/* TFTP error codes */
#define TFTP_ENOTFOUND	1
#define TFTP_EACCESS	2
#define TFTP_EFULL	3
#define TFTP_EILLEGAL	4
#define TFTP_EEXISTS	6

#define DEF_BLKSIZE	512
#define MAX_BLKSIZE	8192
#define MAX_PKT		(MAX_BLKSIZE + 64)
#define MAX_XFERS	8

/* Default service identity, taken from the modem's own log line. */
#define RFS_SERVICE	4096
#define RFS_INSTANCE	3

struct xfer {
	bool active;
	bool writing;			/* WRQ (modem -> us) vs RRQ */
	bool eof;			/* last data block already sent */
	struct sockaddr_qrtr peer;
	FILE *fp;
	unsigned int block;		/* last block sent/received */
	unsigned int blksize;
	unsigned int wsize;		/* RFC 7440 window: blocks per ACK */
	unsigned int win_pending;	/* blocks since the last ACK */
	char path[PATH_MAX];
	time_t last;
};

static const char *g_root = "/var/lib/rfs/mdm/mpss";
static bool g_verbose;
/*
 * Windowed transfer (RFC 7440) is OFF by default. The modem negotiates
 * wsize=10, but honouring it correlated with an ERRFATAL a few seconds into
 * boot, where the same setup with wsize ignored kept the modem up. Declining
 * the option is legal - the client falls back to lock-step - so the safe
 * behaviour is the default and -w is available to re-test it.
 */
static bool g_allow_wsize;
static volatile sig_atomic_t g_stop;
static struct xfer g_xfers[MAX_XFERS];

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static void logmsg(const char *fmt, ...)
{
	struct timespec ts;
	va_list ap;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("[%6ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

static void dbg(const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	va_start(ap, fmt);
	printf("         ");
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
	fflush(stdout);
}

/*
 * Resolve a modem-supplied path beneath our root. The modem sends absolute
 * paths ("/readonly/firmware/...", "/readwrite/mcfg.tmp"); refuse anything
 * containing ".." so it cannot escape the root.
 */
static int resolve_path(const char *req, char *out, size_t outsz)
{
	const char *p = req;

	if (strstr(req, ".."))
		return -1;
	while (*p == '/')
		p++;
	if (snprintf(out, outsz, "%s/%s", g_root, p) >= (int)outsz)
		return -1;
	return 0;
}

static int mkdir_parents(const char *path)
{
	char tmp[PATH_MAX];
	char *slash;

	if (snprintf(tmp, sizeof(tmp), "%s", path) >= (int)sizeof(tmp))
		return -1;

	for (slash = tmp + 1; *slash; slash++) {
		if (*slash != '/')
			continue;
		*slash = '\0';
		mkdir(tmp, 0755);
		*slash = '/';
	}
	return 0;
}

static struct xfer *xfer_find(const struct sockaddr_qrtr *peer)
{
	int i;

	for (i = 0; i < MAX_XFERS; i++)
		if (g_xfers[i].active &&
		    g_xfers[i].peer.sq_node == peer->sq_node &&
		    g_xfers[i].peer.sq_port == peer->sq_port)
			return &g_xfers[i];
	return NULL;
}

static struct xfer *xfer_alloc(const struct sockaddr_qrtr *peer)
{
	struct xfer *x = xfer_find(peer);
	int i;

	if (x) {
		if (x->fp)
			fclose(x->fp);
		memset(x, 0, sizeof(*x));
	} else {
		for (i = 0; i < MAX_XFERS; i++)
			if (!g_xfers[i].active) {
				x = &g_xfers[i];
				break;
			}
		if (!x)
			return NULL;
		memset(x, 0, sizeof(*x));
	}

	x->active = true;
	x->peer = *peer;
	x->blksize = DEF_BLKSIZE;
	x->wsize = 1;
	x->last = time(NULL);
	return x;
}

static void xfer_close(struct xfer *x)
{
	if (!x)
		return;
	if (x->fp)
		fclose(x->fp);
	memset(x, 0, sizeof(*x));
}

static void put16(uint8_t *p, unsigned int v)
{
	p[0] = (v >> 8) & 0xFF;
	p[1] = v & 0xFF;
}

static unsigned int get16(const uint8_t *p)
{
	return ((unsigned int)p[0] << 8) | p[1];
}

static void send_pkt(int fd, const struct sockaddr_qrtr *peer,
		     const void *buf, size_t len)
{
	if (sendto(fd, buf, len, 0, (const struct sockaddr *)peer,
		   sizeof(*peer)) < 0)
		logmsg("sendto(%u:%u) failed: %s", peer->sq_node, peer->sq_port,
		       strerror(errno));
}

static void send_error(int fd, const struct sockaddr_qrtr *peer, int code,
		       const char *msg)
{
	uint8_t pkt[128];
	size_t len;

	put16(pkt, TFTP_ERROR);
	put16(pkt + 2, code);
	len = 4 + snprintf((char *)pkt + 4, sizeof(pkt) - 5, "%s", msg) + 1;
	send_pkt(fd, peer, pkt, len);
	dbg("-> ERROR %d %s", code, msg);
}

static void send_ack(int fd, const struct sockaddr_qrtr *peer, unsigned int blk)
{
	uint8_t pkt[4];

	put16(pkt, TFTP_ACK);
	put16(pkt + 2, blk);
	send_pkt(fd, peer, pkt, sizeof(pkt));
	dbg("-> ACK %u", blk);
}

/* Send one DATA block. Returns false once a short block (EOF) was sent. */
static bool send_one_block(int fd, struct xfer *x)
{
	uint8_t pkt[MAX_PKT];
	size_t n;

	n = fread(pkt + 4, 1, x->blksize, x->fp);
	put16(pkt, TFTP_DATA);
	put16(pkt + 2, x->block + 1);
	send_pkt(fd, &x->peer, pkt, 4 + n);
	dbg("-> DATA blk=%u len=%zu", x->block + 1, n);
	x->block++;
	x->last = time(NULL);

	if (n < x->blksize) {
		x->eof = true;
		return false;
	}
	return true;
}

/*
 * RFC 7440 windowed send: emit up to wsize blocks, then wait for a single ACK
 * covering the window. The modem negotiates wsize=10; with wsize=1 this is
 * plain lock-step TFTP.
 */
static void send_window(int fd, struct xfer *x)
{
	unsigned int i;

	for (i = 0; i < x->wsize; i++) {
		if (!send_one_block(fd, x))
			break;
	}
}

/*
 * Parse RRQ/WRQ options and build the OACK. We honour blksize and echo tsize;
 * anything else is ignored, which is legal - the client falls back to defaults.
 */
static size_t build_oack(uint8_t *out, size_t outsz, const char *opts,
			 size_t optlen, struct xfer *x, long filesize)
{
	size_t pos = 2;
	size_t i = 0;
	bool any = false;

	put16(out, TFTP_OACK);

	while (i < optlen) {
		const char *name = opts + i;
		size_t nlen = strnlen(name, optlen - i);
		const char *val;
		size_t vlen;

		if (i + nlen + 1 >= optlen)
			break;
		val = name + nlen + 1;
		vlen = strnlen(val, optlen - (i + nlen + 1));
		i += nlen + 1 + vlen + 1;

		if (!strcasecmp(name, "blksize")) {
			long v = strtol(val, NULL, 10);

			if (v >= 8 && v <= MAX_BLKSIZE) {
				x->blksize = (unsigned int)v;
				pos += snprintf((char *)out + pos, outsz - pos,
						"blksize") + 1;
				pos += snprintf((char *)out + pos, outsz - pos,
						"%u", x->blksize) + 1;
				any = true;
			}
		} else if (!strcasecmp(name, "wsize")) {
			long v = strtol(val, NULL, 10);

			if (g_allow_wsize && v >= 1 && v <= 64) {
				x->wsize = (unsigned int)v;
				pos += snprintf((char *)out + pos, outsz - pos,
						"wsize") + 1;
				pos += snprintf((char *)out + pos, outsz - pos,
						"%u", x->wsize) + 1;
				any = true;
			}
		} else if (!strcasecmp(name, "tsize") && filesize >= 0) {
			pos += snprintf((char *)out + pos, outsz - pos,
					"tsize") + 1;
			pos += snprintf((char *)out + pos, outsz - pos,
					"%ld", filesize) + 1;
			any = true;
		} else {
			dbg("   ignoring option %s=%s", name, val);
		}
	}

	return any ? pos : 0;
}

static void handle_request(int fd, const struct sockaddr_qrtr *peer,
			   const uint8_t *pkt, size_t len, bool writing)
{
	char resolved[PATH_MAX];
	const char *fname = (const char *)pkt + 2;
	size_t flen = strnlen(fname, len - 2);
	const char *mode;
	size_t mlen;
	struct xfer *x;
	uint8_t oack[MAX_PKT];
	size_t oacklen;
	long filesize = -1;
	struct stat st;

	if (2 + flen + 1 >= len)
		return;
	mode = fname + flen + 1;
	mlen = strnlen(mode, len - (2 + flen + 1));

	logmsg("%s %s (mode=%s) from %u:%u", writing ? "WRQ" : "RRQ", fname,
	       mode, peer->sq_node, peer->sq_port);

	if (resolve_path(fname, resolved, sizeof(resolved)) < 0) {
		send_error(fd, peer, TFTP_EACCESS, "bad path");
		return;
	}

	x = xfer_alloc(peer);
	if (!x) {
		send_error(fd, peer, TFTP_EFULL, "too many transfers");
		return;
	}
	x->writing = writing;
	snprintf(x->path, sizeof(x->path), "%s", resolved);

	if (writing) {
		mkdir_parents(resolved);
		x->fp = fopen(resolved, "wb");
		if (!x->fp) {
			logmsg("  cannot create %s: %s", resolved, strerror(errno));
			send_error(fd, peer, TFTP_EACCESS, strerror(errno));
			xfer_close(x);
			return;
		}
	} else {
		x->fp = fopen(resolved, "rb");
		if (!x->fp) {
			logmsg("  MISSING %s", resolved);
			send_error(fd, peer, TFTP_ENOTFOUND, "not found");
			xfer_close(x);
			return;
		}
		if (stat(resolved, &st) == 0)
			filesize = (long)st.st_size;
	}

	logmsg("  -> %s", resolved);

	oacklen = build_oack(oack, sizeof(oack),
			     mode + mlen + 1, len - (2 + flen + 1 + mlen + 1),
			     x, filesize);
	if (oacklen) {
		send_pkt(fd, peer, oack, oacklen);
		dbg("-> OACK (blksize=%u)", x->blksize);
		/* Client ACKs block 0 before we start sending. */
		if (writing)
			return;
		return;
	}

	if (writing)
		send_ack(fd, peer, 0);
	else
		send_window(fd, x);
}

static void handle_data(int fd, const struct sockaddr_qrtr *peer,
			const uint8_t *pkt, size_t len)
{
	struct xfer *x = xfer_find(peer);
	unsigned int blk = get16(pkt + 2);
	size_t dlen = len - 4;

	if (!x || !x->writing || !x->fp) {
		send_error(fd, peer, TFTP_EILLEGAL, "no transfer");
		return;
	}

	if (blk == x->block + 1) {
		if (dlen && fwrite(pkt + 4, 1, dlen, x->fp) != dlen) {
			logmsg("  write failed on %s: %s", x->path,
			       strerror(errno));
			send_error(fd, peer, TFTP_EFULL, "write failed");
			xfer_close(x);
			return;
		}
		x->block = blk;
		x->last = time(NULL);
	}

	x->win_pending++;

	/* Windowed receive: one ACK per wsize blocks, and always at EOF. */
	if (dlen < x->blksize || x->win_pending >= x->wsize) {
		send_ack(fd, peer, x->block);
		x->win_pending = 0;
	}

	if (dlen < x->blksize) {
		fflush(x->fp);
		logmsg("  WRQ complete: %s (%u blocks)", x->path, x->block);
		xfer_close(x);
	}
}

static void handle_ack(int fd, const struct sockaddr_qrtr *peer,
		       const uint8_t *pkt)
{
	struct xfer *x = xfer_find(peer);
	unsigned int blk = get16(pkt + 2);

	if (!x || x->writing || !x->fp)
		return;

	/* ACK of block 0 acknowledges our OACK and starts the transfer. */
	if (blk == 0 && x->block == 0) {
		send_window(fd, x);
		return;
	}

	/*
	 * Only the ACK for the last block of the window advances us; earlier
	 * ones are duplicates or stragglers and are ignored.
	 */
	if (blk != x->block)
		return;

	if (x->eof) {
		logmsg("  RRQ complete: %s (%u blocks)", x->path, x->block);
		xfer_close(x);
		return;
	}

	send_window(fd, x);
}

static int publish_service(int fd, uint32_t node, uint32_t port,
			   uint32_t service, uint32_t instance)
{
	struct sockaddr_qrtr sq = {
		.sq_family = AF_QIPCRTR,
		.sq_node = node,
		.sq_port = QRTR_PORT_CTRL,
	};
	struct qrtr_ctrl_pkt pkt;

	memset(&pkt, 0, sizeof(pkt));
	pkt.cmd = QRTR_TYPE_NEW_SERVER;
	pkt.server.service = service;
	pkt.server.instance = instance;
	pkt.server.node = node;
	pkt.server.port = port;

	if (sendto(fd, &pkt, sizeof(pkt), 0, (struct sockaddr *)&sq,
		   sizeof(sq)) < 0) {
		logmsg("NEW_SERVER(%u/%u) failed: %s", service, instance,
		       strerror(errno));
		return -1;
	}

	logmsg("published QRTR service %u instance %u at %u:%u", service,
	       instance, node, port);
	return 0;
}

static void usage(const char *a0)
{
	fprintf(stderr,
"Usage: %s [options]\n"
"\n"
"Serves the SDX55's remote filesystem over TFTP-on-QRTR, the job Android\n"
"gives to /vendor/bin/tftp_server. Without it the modem logs\n"
"\"could not resolve remote host\" and every RFS access times out.\n"
"\n"
"  -r, --root <dir>       filesystem root (default %s)\n"
"                         must contain readonly/ readwrite/ shared/\n"
"  -s, --service <n>      QRTR service id (default %d)\n"
"  -i, --instance <n>     QRTR instance id (default %d, from the modem log)\n"
"  -w, --wsize            honour the client's windowsize option (RFC 7440).\n"
"                         OFF by default: enabling it correlated with an\n"
"                         ERRFATAL shortly after boot.\n"
"  -v, --verbose          log every TFTP packet\n"
"  -h, --help             this text\n",
		a0, g_root, RFS_SERVICE, RFS_INSTANCE);
}

int main(int argc, char **argv)
{
	uint32_t service = RFS_SERVICE, instance = RFS_INSTANCE;
	struct sockaddr_qrtr sq;
	socklen_t sl = sizeof(sq);
	int fd, c;

	static const struct option opts[] = {
		{ "root",     required_argument, NULL, 'r' },
		{ "service",  required_argument, NULL, 's' },
		{ "instance", required_argument, NULL, 'i' },
		{ "wsize",    no_argument,       NULL, 'w' },
		{ "verbose",  no_argument,       NULL, 'v' },
		{ "help",     no_argument,       NULL, 'h' },
		{}
	};

	while ((c = getopt_long(argc, argv, "r:s:i:wvh", opts, NULL)) != -1) {
		switch (c) {
		case 'r': g_root = optarg; break;
		case 's': service = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'i': instance = (uint32_t)strtoul(optarg, NULL, 0); break;
		case 'w': g_allow_wsize = true; break;
		case 'v': g_verbose = true; break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 1;
		}
	}

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	fd = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
	if (fd < 0) {
		fprintf(stderr, "socket(AF_QIPCRTR): %s\n", strerror(errno));
		return 1;
	}

	memset(&sq, 0, sizeof(sq));
	sq.sq_family = AF_QIPCRTR;
	sq.sq_node = 1;
	sq.sq_port = 0;		/* auto-assign */
	if (bind(fd, (struct sockaddr *)&sq, sizeof(sq)) < 0) {
		fprintf(stderr, "bind: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	if (getsockname(fd, (struct sockaddr *)&sq, &sl) < 0) {
		fprintf(stderr, "getsockname: %s\n", strerror(errno));
		close(fd);
		return 1;
	}

	logmsg("serving RFS root %s", g_root);
	if (publish_service(fd, sq.sq_node, sq.sq_port, service, instance) < 0) {
		close(fd);
		return 1;
	}

	while (!g_stop) {
		struct sockaddr_qrtr peer;
		socklen_t plen = sizeof(peer);
		uint8_t pkt[MAX_PKT];
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		ssize_t n;
		unsigned int op;

		if (poll(&pfd, 1, 1000) <= 0)
			continue;

		n = recvfrom(fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&peer,
			     &plen);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			logmsg("recvfrom: %s", strerror(errno));
			break;
		}
		if (n < 4)
			continue;

		/* Ignore QRTR control traffic addressed to us. */
		if (peer.sq_port == QRTR_PORT_CTRL) {
			dbg("<- QRTR ctrl packet (%zd bytes)", n);
			continue;
		}

		op = get16(pkt);
		dbg("<- op=%u len=%zd from %u:%u", op, n, peer.sq_node,
		    peer.sq_port);

		switch (op) {
		case TFTP_RRQ:
			handle_request(fd, &peer, pkt, (size_t)n, false);
			break;
		case TFTP_WRQ:
			handle_request(fd, &peer, pkt, (size_t)n, true);
			break;
		case TFTP_DATA:
			handle_data(fd, &peer, pkt, (size_t)n);
			break;
		case TFTP_ACK:
			handle_ack(fd, &peer, pkt);
			break;
		case TFTP_ERROR:
			logmsg("client error %u: %s", get16(pkt + 2),
			       (const char *)pkt + 4);
			xfer_close(xfer_find(&peer));
			break;
		default:
			dbg("   unhandled opcode %u", op);
			break;
		}
	}

	logmsg("shutting down");
	close(fd);
	return 0;
}
