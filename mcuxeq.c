/*
 *  Microcontroller Command/Response Utility
 *
 *  (C) Copyright 2024 Geert Uytterhoeven
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.
 */

#include <cap-ng.h>
#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <bsd/stdlib.h>

#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MCUXEQ_DEV_ENV		"MCUXEQ_DEV"
#define MCUXEQ_PROMPT_ENV	"MCUXEQ_PROMPT"

#define DEFAULT_PROMPT		"^[[:alnum:]]*[#$>] $"
#define DEFAULT_TIMEOUT_MS	2000

#define BUF_SIZE		64
#define LINE_SIZE		1024

#define RETRY_MS		200

static const char *opt_dev;
static const char *opt_prompt;
static int opt_timeout = DEFAULT_TIMEOUT_MS;
static int opt_debug;
static int opt_force;

static regex_t regex_prompt;

#define pr_debug(fmt, ...)	{ if (opt_debug) printf(fmt, ##__VA_ARGS__); }
#define pr_info(fmt, ...)	printf(fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)	fprintf(stderr, fmt, ##__VA_ARGS__)

static inline unsigned char mkprint(unsigned char c)
{
	return isprint(c) ? c : '.';
}

static void pr_hexdump(const unsigned char *buf, unsigned int len)
{
	unsigned int off, i, n;

	for (off = 0; len; off += 16, len -= n) {
		printf("%04x:", off);
		n = len < 16 ? len : 16;
		for (i = 0; i < n; i++)
			printf(" %02x", buf[off + i]);
		for (i = n; i < 16; i++)
			printf("   ");
		printf(" |");
		for (i = 0; i < n; i++)
			printf("%c", mkprint(buf[off + i]));
		for (i = n; i < 16; i++)
			printf(" ");
		printf("|\n");
	}
}

static void __attribute__ ((noreturn)) usage(void)
{
	fprintf(stderr,
		"\n"
		"%s: [options] [--] <command> ...\n\n"
		"Valid options are:\n"
		"    -h, --help              Display this usage information\n"
		"    -s, --device <dev>      Serial device to use\n"
		"                            (Default: value of $%s if set)\n"
		"    -p, --prompt <prompt>   Expected prompt regex\n"
		"                            (Default: value of $%s if set)\n"
		"                            (Default: \"%s\")\n"
		"    -t, --timeout <ms>      Timeout value in milliseconds\n"
		"                            (Default: %u)\n"
		"    -d, --debug             Increase debug level\n"
		"    -f, --force             Force open when busy (needs CAP_SYS_ADMIN)"
		"\n",
		getprogname(), MCUXEQ_DEV_ENV, MCUXEQ_PROMPT_ENV,
		DEFAULT_PROMPT, DEFAULT_TIMEOUT_MS);
	exit(1);
}

static void get_time(struct timeval *tv)
{
	int res;

	res = gettimeofday(tv, NULL);
	if (res < 0) {
		pr_err("Failed to get time: %s\n", strerror(errno));
		exit(-1);
	}
}

static void timeout_init(struct timeval *tv)
{
	if (opt_timeout <= 0)
		return;

	get_time(tv);

	tv->tv_usec += opt_timeout * 1000;

	if (tv->tv_usec >= 1000000) {
		div_t qr = div(tv->tv_usec, 1000000);

		tv->tv_sec += qr.quot;
		tv->tv_usec = qr.rem;
	}
}

static int timed_out(struct timeval *tv)
{
	struct timeval now;

	if (opt_timeout <= 0)
		return 0;

	get_time(&now);

	return (now.tv_sec > tv->tv_sec) ||
	       (now.tv_sec == tv->tv_sec && now.tv_usec > tv->tv_usec);
}

static int ser_open(const char *pathname, int flags)
{
	struct termios termios;
	struct timeval tv;
	int fd;

	if (!opt_force) {
		// Drop CAP_SYS_ADMIN to honor current TIOCEXCL state
		capng_fill(CAPNG_SELECT_BOTH);
		capng_update(CAPNG_DROP, CAPNG_EFFECTIVE, CAP_SYS_ADMIN);
		capng_apply(CAPNG_SELECT_BOTH);
	}

	pr_debug("Opening %s...\n", pathname);
	timeout_init(&tv);
	while (1) {
		fd = open(pathname, flags);
		if (fd >= 0 && (opt_force || !flock(fd,  LOCK_EX | LOCK_NB)))
			break;

		if ((errno != EBUSY && errno != EAGAIN) || timed_out(&tv)) {
			pr_err("Failed to open %s: %s\n", pathname,
			       strerror(errno));
			exit(-1);
		}

		pr_debug("%s, retrying\n", strerror(errno));
		usleep(RETRY_MS * 1000);
	}

	if (ioctl(fd, TIOCEXCL)) {
		pr_err("Failed to put terminal in exclusive mode: %s\n",
		       strerror(errno));
		exit(-1);
	}

	if (tcgetattr(fd, &termios)) {
		pr_err("Failed to get terminal attributes: %s\n",
		       strerror(errno));
		exit(-1);
	}

	cfmakeraw(&termios);
	if (tcsetattr(fd, TCSANOW, &termios)) {
		pr_err("Failed to enable raw mode: %s\n", strerror(errno));
		exit(-1);
	}

	if (tcflush(fd, TCIOFLUSH)) {
		pr_err("Failed to flush: %s\n", strerror(errno));
		exit(-1);
	}

	return fd;
}

static int ser_getc(int fd)
{
	static unsigned char buf[BUF_SIZE];
	static unsigned int pos;
	struct pollfd pfd;
	static ssize_t n;
	int res;

	if (pos >= n) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		res = poll(&pfd, 1, opt_timeout);
		pr_debug("poll() returned %d errno %d revents 0x%x\n", res,
			 errno, pfd.revents);
		if (res < 0) {
			pr_err("Poll error: %s\n", strerror(errno));
			exit(-1);
		}

		if (!res) {
			pr_err("Timeout\n");
			exit(-1);
		}

		n = read(fd, buf, sizeof(buf));
		if (!n) {
			pr_err("No data\n");
			exit(-1);
		}

		if (n < 0) {
			pr_err("Read error: %s\n", strerror(errno));
			exit(-1);
		}

		pos = 0;

		pr_debug("Read %zd bytes\n", n);
		if (opt_debug > 1)
			pr_hexdump(buf, n);
	}

	return buf[pos++];
}

static char *ser_readline(int fd)
{
	static char line[LINE_SIZE];
	unsigned int n = 0;
	int c;

	do {
		do {
			c = ser_getc(fd);
		} while (c == '\r');

		if (n >= sizeof(line) - 1) {
			pr_err("Line too long\n");
			exit(-1);
		}

		line[n++] = c;
		line[n] = '\0';

		if (!regexec(&regex_prompt, line, 0, NULL, 0)) {
			pr_debug("Prompt seen, end of data\n");
			return NULL;
		}
	} while (c != '\n');

	return line;
}

static const char *join_words(char *words[], size_t nwords, size_t *len_out)
{
	unsigned int i, j;
	size_t len;
	char *line;

	for (i = 0, len = 0; i < nwords; i++)
		len += strlen(words[i]) + 1;

	line = malloc(len + 1);
	if (!line) {
		pr_err("Failed to allocate buffer: %s\n", strerror(errno));
		exit(-1);
	}

	for (i = 0, j = 0; i < nwords; i++) {
		size_t n = strlen(words[i]);

		if (i)
			line[j++] = ' ';
		memcpy(line + j, words[i], n);
		j += n;
	}
	line[j++] = '\n';
	line[j++] = '\0';

	*len_out = len;

	return line;
}

int main(int argc, char *argv[])
{
	const char *cmd, *line;
	struct timeval tv;
	int ret, fd;
	ssize_t out;
	size_t len;

	while (argc > 1 && argv[1][0] == '-') {
		if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
			usage();
		} else if (!strcmp(argv[1], "-d") ||
			   !strcmp(argv[1], "--debug")) {
			opt_debug++;
		} else if (!strcmp(argv[1], "-f") ||
			   !strcmp(argv[1], "--force")) {
			opt_force = 1;
		} else if (!strcmp(argv[1], "--")) {
			argv++;
			argc--;
			break;
		} else if (argc > 2) {
			if (!strcmp(argv[1], "-s") ||
			    !strcmp(argv[1], "--device")) {
				opt_dev = argv[2];
			} else if (!strcmp(argv[1], "-p") ||
				   !strcmp(argv[1], "--prompt")) {
				opt_prompt = argv[2];
			} else if (!strcmp(argv[1], "-t") ||
			    !strcmp(argv[1], "--timeout")) {
				opt_timeout = atoi(argv[2]);
			} else {
				usage();
			}
			argv++;
			argc--;
		} else {
			usage();
		}
		argv++;
		argc--;
	}

	if (!opt_dev)
		opt_dev = getenv(MCUXEQ_DEV_ENV);

	if (!opt_prompt)
		opt_prompt = getenv(MCUXEQ_PROMPT_ENV);
	if (!opt_prompt)
		opt_prompt = DEFAULT_PROMPT;

	if (!opt_dev || argc <= 1)
		usage();

	ret = regcomp(&regex_prompt, opt_prompt, REG_NOSUB);
	if (ret) {
		char errbuf[256];

		regerror(ret, &regex_prompt, errbuf, sizeof(errbuf));
		pr_err("Failed to compile prompt regex: %s\n", errbuf);
		exit(-1);
	}

	cmd = join_words(argv + 1, argc - 1, &len);

	fd = ser_open(opt_dev, O_RDWR | O_NOCTTY);

	pr_debug("Sending command...\n");
	out = write(fd, cmd, len);
	if (out < 0) {
		pr_err("Write error: %s\n", strerror(errno));
		exit(-1);
	}
	if (out < len) {
		pr_err("Short write %zd < %zu\n", out, len);
		exit(-1);
	}

	pr_debug("Waiting for command echo...\n");
	timeout_init(&tv);
	while (1) {
		line = ser_readline(fd);
		if (line && strstr(line, cmd))
			break;

		if (!line || timed_out(&tv)) {
			pr_err("Command echo not found\n");
			exit(-1);
		}
		pr_debug("Ignoring %s", line);
	}

	pr_debug("Command echo found.\n");

	timeout_init(&tv);
	while (1) {
		line = ser_readline(fd);
		if (!line)
			break;

		if (timed_out(&tv)) {
			pr_err("Response too long\n");
			exit(-1);
		}

		printf("%s", line);
	}

	close(fd);
	regfree(&regex_prompt);

	exit(0);
}
