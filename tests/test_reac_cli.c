// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>

/* reac_cli -- the parsers main() reads its flags and conf lists with (src/reac_cli.c).
 *
 * They used to be statics inside main.c, so nothing could call them but the whole
 * daemon; the netns scripts reached them only through a running process. This
 * compiles reac_cli.c alone: no PipeWire, no socket.
 *
 * What is pinned is the operator-visible contract of each:
 *   - a MAC is six hex pairs and nothing more;
 *   - an interface list splits on commas and white space, truncates a name to
 *     IFNAMSIZ-1, and REPORTS a token past the bound (never loses it silently);
 *   - a head-amp cell is CH:phantom|pad|sens:VALUE inside the declared ranges, and a
 *     LIST with one bad cell is refused whole, never half applied. */
#include "reac_cli.h"
#include "reac_facts_pw.h"

#include <reac/reac_ctrlblk.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int test_mac(void)
{
	uint8_t m[6];
	CHK(reac_cli_parse_mac("00:1a:2B:3c:4d:5e", m) == 0);
	CHK(m[0] == 0x00 && m[1] == 0x1a && m[2] == 0x2b && m[3] == 0x3c &&
	    m[4] == 0x4d && m[5] == 0x5e);
	CHK(reac_cli_parse_mac("ff:ff:ff:ff:ff:ff", m) == 0 && m[0] == 0xff && m[5] == 0xff);
	CHK(reac_cli_parse_mac("", m) == -1);
	CHK(reac_cli_parse_mac("00:11:22:33:44", m) == -1);        /* five pairs */
	CHK(reac_cli_parse_mac("00-11-22-33-44-55", m) == -1);     /* wrong separator */
	CHK(reac_cli_parse_mac("zz:11:22:33:44:55", m) == -1);     /* not hex */
	return 0;
}

static int test_split_list(void)
{
	char out[4][IFNAMSIZ + 1];
	CHK(reac_cli_split_list(NULL, out, 4) == 0);
	CHK(reac_cli_split_list("", out, 4) == 0);
	CHK(reac_cli_split_list(" ,\t, ", out, 4) == 0);           /* all separators */
	CHK(reac_cli_split_list("eth0", out, 4) == 1 && !strcmp(out[0], "eth0"));
	CHK(reac_cli_split_list("eth0,eth1 eth2\tvlan.5", out, 4) == 4);
	CHK(!strcmp(out[0], "eth0") && !strcmp(out[1], "eth1") &&
	    !strcmp(out[2], "eth2") && !strcmp(out[3], "vlan.5"));
	CHK(reac_cli_split_list(",,eth0,,", out, 4) == 1 && !strcmp(out[0], "eth0"));

	/* A name longer than IFNAMSIZ-1 is cut, and the cut one is still NUL-terminated. */
	char longname[64];
	memset(longname, 'x', sizeof longname - 1);
	longname[sizeof longname - 1] = '\0';
	CHK(reac_cli_split_list(longname, out, 4) == 1);
	CHK(strlen(out[0]) <= IFNAMSIZ && strlen(out[0]) >= IFNAMSIZ - 1);
	return 0;
}

/* The bound: a token past `max` is dropped AND SAID, with the list and the token named. */
static int test_split_list_reports_overflow(void)
{
	char out[2][IFNAMSIZ + 1];
	FILE *tf = tmpfile();
	CHK(tf != NULL);
	int fd = fileno(tf);
	fflush(stderr);
	int saved = dup(2);
	dup2(fd, 2);
	int n = reac_cli_split_list("a,b,c", out, 2);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	char buf[256] = "";
	lseek(fd, 0, SEEK_SET);
	ssize_t got = read(fd, buf, sizeof buf - 1);
	fclose(tf);
	CHK(n == 2 && !strcmp(out[0], "a") && !strcmp(out[1], "b"));
	CHK(got > 0);
	CHK(strstr(buf, "too many entries") && strstr(buf, "'c' is dropped"));
	return 0;
}

static int test_headamp_cell(void)
{
	struct reac_headamp_setting s;
	memset(&s, 0xee, sizeof s);
	CHK(reac_cli_parse_headamp("3:phantom:1", &s) == 0);
	CHK(s.ch == 3 && s.param == REAC_HEADAMP_PHANTOM && s.value == 1);
	CHK(reac_cli_parse_headamp("0:pad:0", &s) == 0);
	CHK(s.ch == 0 && s.param == REAC_HEADAMP_PAD && s.value == 0);
	CHK(reac_cli_parse_headamp("47:sens:55", &s) == 0);   /* last channel, last step */
	CHK(s.ch == REAC_HEADAMP_MAX_CH - 1 && s.param == REAC_HEADAMP_SENS &&
	    s.value == REAC_HEADAMP_SENS_MAX);

	CHK(reac_cli_parse_headamp("48:phantom:1", &s) == -1);          /* channel one past */
	CHK(reac_cli_parse_headamp("0:sens:56", &s) == -1);             /* step one past */
	CHK(reac_cli_parse_headamp("0:phantom:2", &s) == -1);           /* a switch is 0/1 */
	CHK(reac_cli_parse_headamp("0:pad:2", &s) == -1);
	CHK(reac_cli_parse_headamp("0:gain:1", &s) == -1);              /* unknown param */
	CHK(reac_cli_parse_headamp("phantom:1", &s) == -1);             /* no channel */
	CHK(reac_cli_parse_headamp("0:phantom", &s) == -1);             /* no value */
	CHK(reac_cli_parse_headamp("", &s) == -1);
	CHK(reac_cli_parse_headamp("-1:phantom:1", &s) == -1);          /* negative channel */
	CHK(reac_cli_parse_headamp("0:phantom:-1", &s) == -1);          /* negative value */
	return 0;
}

/* A refused cell must not have written the caller's setting. */
static int test_headamp_refusal_leaves_the_setting(void)
{
	struct reac_headamp_setting s = { .ch = 9, .param = 9, .value = 9 };
	CHK(reac_cli_parse_headamp("48:phantom:1", &s) == -1);
	CHK(s.ch == 9 && s.param == 9 && s.value == 9);
	CHK(reac_cli_parse_headamp("0:sens:99", &s) == -1);
	CHK(s.ch == 9 && s.param == 9 && s.value == 9);
	return 0;
}

static int test_headamp_list(void)
{
	struct reac_headamp_setting t[4];
	CHK(reac_cli_parse_headamp_list("", t, 4) == 0);
	CHK(reac_cli_parse_headamp_list(" , ", t, 4) == 0);
	CHK(reac_cli_parse_headamp_list("0:phantom:1,1:pad:1 2:sens:10", t, 4) == 3);
	CHK(t[0].ch == 0 && t[0].param == REAC_HEADAMP_PHANTOM && t[0].value == 1);
	CHK(t[1].ch == 1 && t[1].param == REAC_HEADAMP_PAD && t[1].value == 1);
	CHK(t[2].ch == 2 && t[2].param == REAC_HEADAMP_SENS && t[2].value == 10);

	/* One bad cell refuses the WHOLE list. */
	CHK(reac_cli_parse_headamp_list("0:phantom:1,1:pad:9", t, 4) == -1);
	CHK(reac_cli_parse_headamp_list("0:phantom:1 junk", t, 4) == -1);
	/* The bound is `max` cells: the (max+1)th is a refusal, not a silent drop. */
	CHK(reac_cli_parse_headamp_list("0:pad:0,1:pad:0,2:pad:0,3:pad:0", t, 4) == 4);
	CHK(reac_cli_parse_headamp_list("0:pad:0,1:pad:0,2:pad:0,3:pad:0,4:pad:0", t, 4) == -1);
	/* A cell too long for the parser's buffer is a refusal. */
	char big[80];
	memset(big, '1', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	CHK(reac_cli_parse_headamp_list(big, t, 4) == -1);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= test_mac();
	rc |= test_split_list();
	rc |= test_split_list_reports_overflow();
	rc |= test_headamp_cell();
	rc |= test_headamp_refusal_leaves_the_setting();
	rc |= test_headamp_list();
	if (rc == 0)
		printf("test_reac_cli: ok\n");
	return rc;
}
