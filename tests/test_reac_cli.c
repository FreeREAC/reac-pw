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
#include <reac/reac_master.h>
#include <reac/transport/reac_slave.h>

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* A NUMBER THE PROTOCOL DECLARES IS NEVER SPELLED HERE: tools/facts-perturb-check.sh builds
 * this against a fictional protocol, and a literal that agreed with the real one goes red.
 * F() formats a command or conf string from the declared names. */
#include <stdarg.h>
static const char *F(const char *fmt, ...)
{
	static char ring[8][160];
	static unsigned at;
	char *o = ring[at++ & 7];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(o, sizeof ring[0], fmt, ap);
	va_end(ap);
	return o;
}
#define R44 REAC_SAMPLE_RATE_44K1
#define R48 REAC_SAMPLE_RATE_48K
#define R96 REAC_SAMPLE_RATE_96K
#define RBAD (REAC_SAMPLE_RATE_96K + 1)
/* The width cases divide by the declared pair width, as the product does. A fictional protocol
 * that declares a pair narrower than two (the perturbed sets do: 0) makes the product's own
 * `% REAC_BRAID_PAIR_CHANNELS` trap before any assertion, so those cases run only on a
 * protocol with a real pair, and say so when they do not. */
#define PAIR REAC_BRAID_PAIR_CHANNELS
#define WIDTH_CASES_APPLY() (PAIR >= 2 ? 1 : (printf("test_reac_cli: width cases skipped, the declared pair is %d\n", PAIR), 0))
#define WMAX ((REAC_MAX_CHANNELS / PAIR) * PAIR)

#define CHK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s (line %d)\n", #c, __LINE__); return 1; } } while (0)

static int test_int(void)
{
	int v = 77;
	CHK(reac_cli_parse_int("48000", &v) == 0 && v == 48000);
	CHK(reac_cli_parse_int("-8", &v) == 0 && v == -8);
	CHK(reac_cli_parse_int("0", &v) == 0 && v == 0);
	v = 77;
	CHK(reac_cli_parse_int("", &v) == -1);
	CHK(reac_cli_parse_int(NULL, &v) == -1);
	CHK(reac_cli_parse_int("8x", &v) == -1);
	CHK(reac_cli_parse_int(" 8", &v) == -1);
	CHK(reac_cli_parse_int("8 ", &v) == -1);
	CHK(reac_cli_parse_int("+8", &v) == -1);
	CHK(reac_cli_parse_int("-", &v) == -1);
	CHK(reac_cli_parse_int("99999999999", &v) == -1);   /* past int */
	CHK(v == 77);                                       /* a refusal leaves it alone */
	return 0;
}

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
	CHK(reac_cli_parse_headamp(F("%d:sens:%d", REAC_HEADAMP_MAX_CH - 1, REAC_HEADAMP_SENS_MAX), &s) == 0);   /* last channel, last step */
	CHK(s.ch == REAC_HEADAMP_MAX_CH - 1 && s.param == REAC_HEADAMP_SENS &&
	    s.value == REAC_HEADAMP_SENS_MAX);

	CHK(reac_cli_parse_headamp(F("%d:phantom:1", REAC_HEADAMP_MAX_CH), &s) == -1);          /* channel one past */
	CHK(reac_cli_parse_headamp(F("0:sens:%d", REAC_HEADAMP_SENS_MAX + 1), &s) == -1);             /* step one past */
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
	CHK(reac_cli_parse_headamp(F("%d:phantom:1", REAC_HEADAMP_MAX_CH), &s) == -1);
	CHK(s.ch == 9 && s.param == 9 && s.value == 9);
	CHK(reac_cli_parse_headamp(F("0:sens:%d", REAC_HEADAMP_SENS_MAX + 40), &s) == -1);
	CHK(s.ch == 9 && s.param == 9 && s.value == 9);
	return 0;
}

static int test_headamp_list(void)
{
	struct reac_headamp_setting t[4];
	CHK(reac_cli_parse_headamp_list("", t, 4) == 0);
	CHK(reac_cli_parse_headamp_list(" , ", t, 4) == 0);
	CHK(reac_cli_parse_headamp_list(F("0:phantom:1,1:pad:1 2:sens:%d", REAC_HEADAMP_SENS_MAX / 2), t, 4) == 3);
	CHK(t[0].ch == 0 && t[0].param == REAC_HEADAMP_PHANTOM && t[0].value == 1);
	CHK(t[1].ch == 1 && t[1].param == REAC_HEADAMP_PAD && t[1].value == 1);
	CHK(t[2].ch == 2 && t[2].param == REAC_HEADAMP_SENS && t[2].value == REAC_HEADAMP_SENS_MAX / 2);

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

/* TRAILING TEXT IS NOT PART OF A VALUE. sscanf stops at the last conversion and says
 * nothing about what is left, so "00:11:22:33:44:55zz" and "0:phantom:1x" were taken
 * as the clean value in front of the junk: a typo on the command line started the
 * daemon with a setting the operator did not type. */
static int test_trailing_text_is_refused(void)
{
	uint8_t m[6];
	struct reac_headamp_setting s, t[2];
	CHK(reac_cli_parse_mac("00:11:22:33:44:55zz", m) == -1);
	CHK(reac_cli_parse_mac("00:11:22:33:44:55:66", m) == -1);
	CHK(reac_cli_parse_mac("00:11:22:33:44:55 ", m) == -1);
	CHK(reac_cli_parse_headamp("0:phantom:1x", &s) == -1);
	CHK(reac_cli_parse_headamp("0:pad:1:extra", &s) == -1);
	CHK(reac_cli_parse_headamp_list("0:phantom:1x", t, 2) == -1);
	return 0;
}

/* ---- the whole command line: reac_cli_parse ---------------------------------- */

/* Run reac_cli_parse over a space-separated line, with stderr captured into `err`. */
static int parse_line(const char *line, struct reac_cli *c, char *err, size_t errsz)
{
	static char buf[512];
	char *argv[40];
	int argc = 0;
	snprintf(buf, sizeof buf, "reac-pw %s", line);
	for (char *t = strtok(buf, " "); t && argc < 39; t = strtok(NULL, " "))
		argv[argc++] = t;
	argv[argc] = NULL;

	FILE *tf = tmpfile();
	if (!tf)
		return -99;
	fflush(stderr);
	int saved = dup(2);
	dup2(fileno(tf), 2);
	int rc = reac_cli_parse(argc, argv, c);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	if (err && errsz) {
		rewind(tf);
		size_t got = fread(err, 1, errsz - 1, tf);
		err[got] = '\0';
	}
	fclose(tf);
	return rc;
}

static int test_defaults(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("", &c, err, sizeof err) == 0);
	CHK(c.role == REAC_ROLE_MASTER && !c.role_tap);       /* default master */
	CHK(c.rxcfg.source == NULL && c.rxcfg.kind == REAC_RX_PCAP);
	CHK(c.rxcfg.forced_rate == 0 && c.rate_layer == REAC_CONF_NONE);
	CHK(c.box_channels == REAC_SLAVE_BOX_CHANNELS_DEFAULT);
	CHK(c.mixer && !strcmp(c.mixer->name, "m200"));
	CHK(!c.tx_if && !c.pin_model && !c.inst_name && !c.src_mac_set);
	CHK(c.n_headamps == 0 && c.n_extra_ifaces == 0 && !c.have_live);
	CHK(err[0] == '\0');
	return 0;
}

static int test_live_interfaces(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--live eth0", &c, err, sizeof err) == 0);
	CHK(c.have_live && c.rxcfg.kind == REAC_RX_LIVE && !strcmp(c.rxcfg.source, "eth0"));
	CHK(c.n_extra_ifaces == 0);
	/* The first interface is the template's own, a stable COPY, not a pointer into argv. */
	CHK(c.rxcfg.source == c.first_iface_buf);

	CHK(parse_line("--live eth0,eth1,eth2 --live eth3", &c, err, sizeof err) == 0);
	CHK(!strcmp(c.rxcfg.source, "eth0") && c.n_extra_ifaces == 3);
	CHK(!strcmp(c.extra_ifaces[0], "eth1") && !strcmp(c.extra_ifaces[1], "eth2") &&
	    !strcmp(c.extra_ifaces[2], "eth3"));

	CHK(parse_line("--live ,", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "--live needs at least one interface name"));

	/* REAC_PW_MAX_LISTENERS - 1 extras fit; the next is dropped AND SAID. */
	CHK(parse_line("--live a,b,c,d,e,f,g,h --live i", &c, err, sizeof err) == 0);
	CHK(c.n_extra_ifaces == REAC_PW_MAX_LISTENERS - 1);
	CHK(strstr(err, "too many --live interfaces") && strstr(err, "'i' is dropped"));
	/* One list past the bound is cut by the splitter, with its own report. */
	CHK(parse_line("--live a,b,c,d,e,f,g,h,i", &c, err, sizeof err) == 0);
	CHK(c.n_extra_ifaces == REAC_PW_MAX_LISTENERS - 1);
	CHK(strstr(err, "too many entries") && strstr(err, "'i' is dropped"));
	CHK(parse_line("--live a,b,c,d,e,f,g,h", &c, err, sizeof err) == 0);
	CHK(c.n_extra_ifaces == REAC_PW_MAX_LISTENERS - 1 && err[0] == '\0');

	CHK(parse_line("--pcap some.pcap", &c, err, sizeof err) == 0);
	CHK(c.rxcfg.kind == REAC_RX_PCAP && !strcmp(c.rxcfg.source, "some.pcap") && !c.have_live);
	return 0;
}

/* Only the three paces REAC has are legal, and the layer says the command line decided. */
static int test_rate(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line(F("--rate %d", R44), &c, err, sizeof err) == 0 && c.rxcfg.forced_rate == R44);
	CHK(c.rate_layer == REAC_CONF_ARGV);
	CHK(parse_line(F("--rate %d", R48), &c, err, sizeof err) == 0 && c.rxcfg.forced_rate == R48);
	CHK(parse_line(F("--rate %d", R96), &c, err, sizeof err) == 0 && c.rxcfg.forced_rate == R96);
	CHK(parse_line(F("--rate %d", RBAD), &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, F("illegal --rate '%d'", RBAD)) && strstr(err, "re-pacing"));
	CHK(parse_line("--rate 0", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(parse_line(F("--rate -%d", R48), &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(parse_line("--rate abc", &c, err, sizeof err) == REAC_CLI_REFUSED);
	return 0;
}

static int test_roles(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--role master", &c, err, sizeof err) == 0 && c.role == REAC_ROLE_MASTER);
	CHK(parse_line("--role tap", &c, err, sizeof err) == 0 && c.role_tap == 1);
	/* tap rides BESIDE role, and a later explicit role clears it. */
	CHK(parse_line("--role tap --role master", &c, err, sizeof err) == 0 && !c.role_tap);
	CHK(parse_line("--role weird", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "unknown --role 'weird' (master|slave|tap)"));

	/* A slave is the box: it needs the NIC to answer on. */
	CHK(parse_line("--role slave", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "--role slave needs --tx IFNAME"));
	CHK(parse_line("--role slave --tx eth1", &c, err, sizeof err) == 0);
	CHK(c.role == REAC_ROLE_SLAVE && !strcmp(c.tx_if, "eth1"));
	return 0;
}

/* --box and --headamp are MASTER options; as a slave we ARE the box. */
static int test_slave_refuses_master_options(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--role slave --tx eth1 --box s1608", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "--box is a MASTER-role option"));
	CHK(parse_line("--role slave --tx eth1 --headamp 0:phantom:1", &c, err, sizeof err) ==
	    REAC_CLI_REFUSED);
	CHK(strstr(err, "--headamp is a master-role option"));
	/* The same two are fine on a master, in either flag order. */
	CHK(parse_line("--box s1608 --role master", &c, err, sizeof err) == 0 && c.pin_model);
	CHK(parse_line("--headamp 0:phantom:1 --role master", &c, err, sizeof err) == 0 &&
	    c.n_headamps == 1);
	/* ...and the order does not let a slave through. */
	CHK(parse_line("--box s1608 --role slave --tx eth1", &c, err, sizeof err) == REAC_CLI_REFUSED);
	return 0;
}

static int test_box_pin_and_names(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--box s1608:Drums", &c, err, sizeof err) == 0);
	CHK(c.pin_model && !strcmp(c.pin_model->token, "s1608"));
	CHK(c.pin_label && !strcmp(c.pin_label, "Drums"));
	CHK(c.box_pin_spec && !strcmp(c.box_pin_spec, "s1608:Drums"));   /* verbatim */
	CHK(parse_line("--box nosuch", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "unknown --box model 'nosuch'") && strstr(err, "s1608") &&
	    strstr(err, "MODEL[:LABEL]"));

	CHK(parse_line("--name stage-left", &c, err, sizeof err) == 0 &&
	    !strcmp(c.inst_name, "stage-left"));
	CHK(parse_line("--mixer m300", &c, err, sizeof err) == 0 && !strcmp(c.mixer->name, "m300"));
	CHK(parse_line("--mixer nosuch", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "unknown --mixer 'nosuch'") && strstr(err, "m200"));
	CHK(parse_line("--src-mac 02:11:22:33:44:55", &c, err, sizeof err) == 0 && c.src_mac_set);
	CHK(c.src_mac[0] == 0x02 && c.src_mac[5] == 0x55);
	CHK(parse_line("--src-mac nonsense", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "bad --src-mac 'nonsense'"));
	return 0;
}

/* A slave's own width: a model token, or an even count within the braid. */
static int test_slave_width(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--box-model s4000s", &c, err, sizeof err) == 0 && c.box_channels == 32);
	CHK(parse_line("--box-model s0808", &c, err, sizeof err) == 0 && c.box_channels == 8);
	CHK(parse_line("--box-model nosuch", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "unknown --box-model 'nosuch'") && strstr(err, "s4000s"));
	if (!WIDTH_CASES_APPLY())
		return 0;
	CHK(parse_line(F("--box-channels %d", PAIR), &c, err, sizeof err) == 0 && c.box_channels == PAIR);
	CHK(parse_line(F("--box-channels %d", WMAX), &c, err, sizeof err) == 0 && c.box_channels == WMAX);
	CHK(parse_line("--box-channels 0", &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(parse_line(F("--box-channels %d", PAIR + 1), &c, err, sizeof err) == REAC_CLI_REFUSED);   /* not a pair multiple */
	CHK(parse_line(F("--box-channels %d", WMAX + PAIR), &c, err, sizeof err) == REAC_CLI_REFUSED);  /* past the fabric */
	CHK(strstr(err, "--box-channels must be even"));
	CHK(parse_line(F("--box-channels -%d", 4 * PAIR), &c, err, sizeof err) == REAC_CLI_REFUSED);
	return 0;
}

static int test_headamp_flag(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line(F("--headamp 1:phantom:1 --headamp 2:sens:%d", REAC_HEADAMP_SENS_MAX / 2), &c, err, sizeof err) == 0);
	CHK(c.n_headamps == 2 && c.headamps[0].ch == 1 && c.headamps[1].ch == 2 &&
	    c.headamps[1].value == REAC_HEADAMP_SENS_MAX / 2);
	CHK(parse_line(F("--headamp %d:phantom:1", REAC_HEADAMP_MAX_CH + 51), &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "bad --headamp '99:phantom:1'"));

	/* The table holds one slot per (channel, param); a flag past it is refused, not dropped. */
	/* parse_line tokenises into at most 39 args: drive the bound with a real argv. */
	char *argv[2 * REAC_CLI_MAX_HEADAMPS + 4];
	int argc = 0;
	argv[argc++] = "reac-pw";
	for (int i = 0; i <= REAC_CLI_MAX_HEADAMPS; i++) {
		argv[argc++] = "--headamp";
		argv[argc++] = "0:pad:0";
	}
	argv[argc] = NULL;
	FILE *tf = tmpfile();
	CHK(tf != NULL);
	fflush(stderr);
	int saved = dup(2);
	dup2(fileno(tf), 2);
	int rc = reac_cli_parse(argc, argv, &c);
	fflush(stderr);
	dup2(saved, 2);
	close(saved);
	fclose(tf);
	CHK(rc == REAC_CLI_REFUSED);
	return 0;
}

/* --set is consumed by the knob pre-pass; the flag loop only steps over its KEY=VALUE.
 * It once fell through to usage() and the daemon exited 2 on a flag --help documents. */
static int test_set_is_stepped_over(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--set REACPW_X=1 --role master", &c, err, sizeof err) == 0);
	CHK(c.role == REAC_ROLE_MASTER && err[0] == '\0');
	CHK(parse_line(F("--live eth0 --set REACPW_X=1 --set REACPW_Y=2 --rate %d", R48), &c, err,
	               sizeof err) == 0);
	CHK(c.rxcfg.forced_rate == R48 && !strcmp(c.rxcfg.source, "eth0"));
	return 0;
}

/* Anything else is the CALLER's usage text, not a refusal message of ours. */
static int test_unknown_flag_is_usage(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line("--nope", &c, err, sizeof err) == REAC_CLI_USAGE);
	CHK(err[0] == '\0');
	CHK(parse_line("positional", &c, err, sizeof err) == REAC_CLI_USAGE);
	/* A value flag with no value falls to usage, never reads past argv. */
	CHK(parse_line("--rate", &c, err, sizeof err) == REAC_CLI_USAGE);
	CHK(parse_line("--live", &c, err, sizeof err) == REAC_CLI_USAGE);
	CHK(parse_line("--role", &c, err, sizeof err) == REAC_CLI_USAGE);
	return 0;
}

/* A NUMBER IS ALL DIGITS. atoi() read the digits in front of the first non-digit and
 * said nothing, so `--rate 48000abc` was the legal 48000 and `--box-channels 8x` was an
 * 8-wide box: the typo ran the daemon at a value the operator did not type. */
static int test_numbers_are_whole(void)
{
	struct reac_cli c;
	char err[1024];
	CHK(parse_line(F("--rate %dabc", R48), &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, F("illegal --rate '%dabc'", R48)));
	CHK(parse_line("--rate 4800.0", &c, err, sizeof err) == REAC_CLI_REFUSED);
	if (!WIDTH_CASES_APPLY())
		return 0;
	CHK(parse_line(F("--box-channels %dx", PAIR), &c, err, sizeof err) == REAC_CLI_REFUSED);
	CHK(strstr(err, "--box-channels must be even"));
	CHK(parse_line(F("--box-channels %d.0", PAIR), &c, err, sizeof err) == REAC_CLI_REFUSED);
	return 0;
}

int main(void)
{
	int rc = 0;
	rc |= test_int();
	rc |= test_mac();
	rc |= test_split_list();
	rc |= test_split_list_reports_overflow();
	rc |= test_headamp_cell();
	rc |= test_headamp_refusal_leaves_the_setting();
	rc |= test_headamp_list();
	rc |= test_trailing_text_is_refused();
	rc |= test_defaults();
	rc |= test_live_interfaces();
	rc |= test_rate();
	rc |= test_roles();
	rc |= test_slave_refuses_master_options();
	rc |= test_box_pin_and_names();
	rc |= test_slave_width();
	rc |= test_headamp_flag();
	rc |= test_set_is_stepped_over();
	rc |= test_unknown_flag_is_usage();
	rc |= test_numbers_are_whole();
	if (rc == 0)
		printf("test_reac_cli: ok\n");
	return rc;
}
