#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <getopt.h>
#include <iptables.h>
#include <xtables.h>

#include <netinet/ether.h>

#include <linux/netfilter_bridge.h>
#include <linux/netfilter/nf_tables.h>
#include <ebtables/ethernetdb.h>
#include <libiptc/libxtc.h>

#include "xshared.h"
#include "xtables-multi.h"
#include "nft-bridge.h"
#include "nft.h"
#include "nft-shared.h"
/*
 * From include/ebtables_u.h
 */
#define EXEC_STYLE_PRG    0
#define EXEC_STYLE_DAEMON 1

#define ebt_check_option2(flags, mask) EBT_CHECK_OPTION(flags, mask)

extern int ebt_invert;

static int ebt_check_inverse2(const char option[], int argc, char **argv)
{
	if (!option)
		return ebt_invert;
	if (strcmp(option, "!") == 0) {
		if (ebt_invert == 1)
			xtables_error(PARAMETER_PROBLEM,
				      "Double use of '!' not allowed");
		if (optind >= argc)
			optarg = NULL;
		else
			optarg = argv[optind];
		optind++;
		ebt_invert = 1;
		return 1;
	}
	return ebt_invert;
}

/*
 * Glue code to use libxtables
 */
static int parse_rule_number(const char *rule)
{
	unsigned int rule_nr;

	if (!xtables_strtoui(rule, NULL, &rule_nr, 1, INT_MAX))
		xtables_error(PARAMETER_PROBLEM,
			      "Invalid rule number `%s'", rule);

	return rule_nr;
}

static const char *
parse_target(const char *targetname)
{
	const char *ptr;

	if (strlen(targetname) < 1)
		xtables_error(PARAMETER_PROBLEM,
			      "Invalid target name (too short)");

	if (strlen(targetname)+1 > EBT_CHAIN_MAXNAMELEN)
		xtables_error(PARAMETER_PROBLEM,
			      "Invalid target '%s' (%d chars max)",
			      targetname, EBT_CHAIN_MAXNAMELEN);

	for (ptr = targetname; *ptr; ptr++)
		if (isspace(*ptr))
			xtables_error(PARAMETER_PROBLEM,
				      "Invalid target name `%s'", targetname);
	return targetname;
}

static int get_current_chain(const char *chain)
{
	if (strcmp(chain, "PREROUTING") == 0)
		return NF_BR_PRE_ROUTING;
	else if (strcmp(chain, "INPUT") == 0)
		return NF_BR_LOCAL_IN;
	else if (strcmp(chain, "FORWARD") == 0)
		return NF_BR_FORWARD;
	else if (strcmp(chain, "OUTPUT") == 0)
		return NF_BR_LOCAL_OUT;
	else if (strcmp(chain, "POSTROUTING") == 0)
		return NF_BR_POST_ROUTING;

	return -1;
}

/*
 * The original ebtables parser
 */

/* Checks whether a command has already been specified */
#define OPT_COMMANDS (flags & OPT_COMMAND || flags & OPT_ZERO)

#define OPT_COMMAND	0x01
#define OPT_TABLE	0x02
#define OPT_IN		0x04
#define OPT_OUT		0x08
#define OPT_JUMP	0x10
#define OPT_PROTOCOL	0x20
#define OPT_SOURCE	0x40
#define OPT_DEST	0x80
#define OPT_ZERO	0x100
#define OPT_LOGICALIN	0x200
#define OPT_LOGICALOUT	0x400
#define OPT_COUNT	0x1000 /* This value is also defined in libebtc.c */

/* Default command line options. Do not mess around with the already
 * assigned numbers unless you know what you are doing */
extern struct option ebt_original_options[];
extern struct xtables_globals ebtables_globals;
#define opts ebtables_globals.opts
#define prog_name ebtables_globals.program_name
#define prog_vers ebtables_globals.program_version

#define OPTION_OFFSET 256
static struct option *merge_options(struct option *oldopts,
				    const struct option *newopts,
				    unsigned int *options_offset)
{
	unsigned int num_old, num_new, i;
	struct option *merge;

	if (!newopts || !oldopts || !options_offset)
		return oldopts;
	for (num_old = 0; oldopts[num_old].name; num_old++);
	for (num_new = 0; newopts[num_new].name; num_new++);

	ebtables_globals.option_offset += OPTION_OFFSET;
	*options_offset = ebtables_globals.option_offset;

	merge = malloc(sizeof(struct option) * (num_new + num_old + 1));
	if (!merge)
		return NULL;
	memcpy(merge, oldopts, num_old * sizeof(struct option));
	for (i = 0; i < num_new; i++) {
		merge[num_old + i] = newopts[i];
		merge[num_old + i].val += *options_offset;
	}
	memset(merge + num_old + num_new, 0, sizeof(struct option));
	/* Only free dynamically allocated stuff */
	if (oldopts != ebt_original_options)
		free(oldopts);

	return merge;
}

/*
 * More glue code.
 */
static struct xtables_target *command_jump(struct ebtables_command_state *cs,
					   const char *jumpto)
{
	struct xtables_target *target;
	size_t size;

	/* XTF_TRY_LOAD (may be chain name) */
	target = xtables_find_target(jumpto, XTF_TRY_LOAD);

	if (!target)
		return NULL;

	size = XT_ALIGN(sizeof(struct xt_entry_target))
		+ target->size;

	target->t = xtables_calloc(1, size);
	target->t->u.target_size = size;
	strncpy(target->t->u.user.name, jumpto, sizeof(target->t->u.user.name));
	target->t->u.user.name[sizeof(target->t->u.user.name)-1] = '\0';
	target->t->u.user.revision = target->revision;

	xs_init_target(target);

	opts = merge_options(opts, target->extra_opts, &target->option_offset);
	if (opts == NULL)
		xtables_error(OTHER_PROBLEM, "Can't alloc memory");

	return target;
}

static void print_help(void)
{
	fprintf(stderr, "%s: Translate ebtables command to nft syntax\n"
			"no side effects occur, the translated command is written "
			"to standard output.\n"
			"A '#' followed by input means no translation "
			"is available.\n", prog_name);
	exit(0);
}

static int parse_rule_range(const char *argv, int *rule_nr, int *rule_nr_end)
{
	char *colon = strchr(argv, ':'), *buffer;

	if (colon) {
		*colon = '\0';
		if (*(colon + 1) == '\0')
			*rule_nr_end = -1; /* Until the last rule */
		else {
			*rule_nr_end = strtol(colon + 1, &buffer, 10);
			if (*buffer != '\0' || *rule_nr_end == 0)
				return -1;
		}
	}
	if (colon == argv)
		*rule_nr = 1; /* Beginning with the first rule */
	else {
		*rule_nr = strtol(argv, &buffer, 10);
		if (*buffer != '\0' || *rule_nr == 0)
			return -1;
	}
	if (!colon)
		*rule_nr_end = *rule_nr;
	return 0;
}

/* Incrementing or decrementing rules in daemon mode is not supported as the
 * involved code overload is not worth it (too annoying to take the increased
 * counters in the kernel into account). */
static int parse_change_counters_rule(int argc, char **argv, int *rule_nr, int *rule_nr_end, int exec_style, struct ebtables_command_state *cs)
{
	char *buffer;
	int ret = 0;

	if (optind + 1 >= argc || (argv[optind][0] == '-' && (argv[optind][1] < '0' || argv[optind][1] > '9')) ||
	    (argv[optind + 1][0] == '-' && (argv[optind + 1][1] < '0'  && argv[optind + 1][1] > '9')))
		xtables_error(PARAMETER_PROBLEM,
			      "The command -C needs at least 2 arguments");
	if (optind + 2 < argc && (argv[optind + 2][0] != '-' || (argv[optind + 2][1] >= '0' && argv[optind + 2][1] <= '9'))) {
		if (optind + 3 != argc)
			xtables_error(PARAMETER_PROBLEM,
				      "No extra options allowed with -C start_nr[:end_nr] pcnt bcnt");
		if (parse_rule_range(argv[optind], rule_nr, rule_nr_end))
			xtables_error(PARAMETER_PROBLEM,
				      "Something is wrong with the rule number specification '%s'", argv[optind]);
		optind++;
	}

	if (argv[optind][0] == '+') {
		if (exec_style == EXEC_STYLE_DAEMON)
daemon_incr:
			xtables_error(PARAMETER_PROBLEM,
				      "Incrementing rule counters (%s) not allowed in daemon mode", argv[optind]);
		ret += 1;
		cs->counters.pcnt = strtoull(argv[optind] + 1, &buffer, 10);
	} else if (argv[optind][0] == '-') {
		if (exec_style == EXEC_STYLE_DAEMON)
daemon_decr:
			xtables_error(PARAMETER_PROBLEM,
				      "Decrementing rule counters (%s) not allowed in daemon mode", argv[optind]);
		ret += 2;
		cs->counters.pcnt = strtoull(argv[optind] + 1, &buffer, 10);
	} else
		cs->counters.pcnt = strtoull(argv[optind], &buffer, 10);

	if (*buffer != '\0')
		goto invalid;
	optind++;
	if (argv[optind][0] == '+') {
		if (exec_style == EXEC_STYLE_DAEMON)
			goto daemon_incr;
		ret += 3;
		cs->counters.bcnt = strtoull(argv[optind] + 1, &buffer, 10);
	} else if (argv[optind][0] == '-') {
		if (exec_style == EXEC_STYLE_DAEMON)
			goto daemon_decr;
		ret += 6;
		cs->counters.bcnt = strtoull(argv[optind] + 1, &buffer, 10);
	} else
		cs->counters.bcnt = strtoull(argv[optind], &buffer, 10);

	if (*buffer != '\0')
		goto invalid;
	optind++;
	return ret;
invalid:
	xtables_error(PARAMETER_PROBLEM,"Packet counter '%s' invalid", argv[optind]);
}

static int parse_iface(char *iface, char *option)
{
	char *c;

	if ((c = strchr(iface, '+'))) {
		if (*(c + 1) != '\0') {
			xtables_error(PARAMETER_PROBLEM,
				      "Spurious characters after '+' wildcard for '%s'", option);
			return -1;
		} else
			*c = IF_WILDCARD;
	}
	return 0;
}

static void print_ebt_cmd(int argc, char *argv[])
{
	int i;

	printf("# ");
	for (i = 1; i < argc; i++)
		printf("%s ", argv[i]);

	printf("\n");
}

static int nft_rule_eb_xlate_add(struct nft_handle *h, const struct nft_xt_cmd_parse *p,
				 const struct ebtables_command_state *cs, bool append)
{
	struct xt_xlate *xl = xt_xlate_alloc(10240);
	int ret;

	if (append) {
		xt_xlate_add(xl, "add rule bridge %s %s ", p->table, p->chain);
	} else {
		xt_xlate_add(xl, "insert rule bridge %s %s ", p->table, p->chain);
	}

	ret = h->ops->xlate(cs, xl);
	if (ret)
		printf("%s\n", xt_xlate_get(xl));

	xt_xlate_free(xl);
	return ret;
}

/* We use exec_style instead of #ifdef's because ebtables.so is a shared object. */
static int do_commandeb_xlate(struct nft_handle *h, int argc, char *argv[], char **table)
{
	char *buffer;
	int c, i;
	int chcounter = 0; /* Needed for -C */
	int rule_nr = 0;
	int rule_nr_end = 0;
	int ret = 0;
	unsigned int flags = 0;
	struct xtables_target *t, *w;
	struct xtables_match *m;
	struct ebtables_command_state cs;
	char command = 'h';
	const char *chain = NULL;
	int exec_style = EXEC_STYLE_PRG;
	int selected_chain = -1;
	struct xtables_rule_match *xtrm_i;
	struct ebt_match *match;
	struct nft_xt_cmd_parse p = {
		.table          = *table,
        };

	memset(&cs, 0, sizeof(cs));
	cs.argv = argv;

	if (nft_init(h, xtables_bridge) < 0)
		xtables_error(OTHER_PROBLEM,
			      "Could not initialize nftables layer.");

	h->ops = nft_family_ops_lookup(h->family);
	if (h->ops == NULL)
		xtables_error(PARAMETER_PROBLEM, "Unknown family");

	/* manually registering ebt matches, given the original ebtables parser
	 * don't use '-m matchname' and the match can't loaded dinamically when
	 * the user calls it.
	 */
	ebt_load_match_extensions();

	/* clear mflags in case do_commandeb gets called a second time
	 * (we clear the global list of all matches for security)*/
	for (m = xtables_matches; m; m = m->next)
		m->mflags = 0;

	for (t = xtables_targets; t; t = t->next) {
		t->tflags = 0;
		t->used = 0;
	}

	/* prevent getopt to spoil our error reporting */
	opterr = false;

	printf("nft ");
	/* Getopt saves the day */
	while ((c = getopt_long(argc, argv,
	   "-A:D:C:I:N:E:X::L::Z::F::P:Vhi:o:j:c:p:s:d:t:M:", opts, NULL)) != -1) {
		cs.c = c;
		cs.invert = ebt_invert;
		switch (c) {
		case 'A': /* Add a rule */
		case 'D': /* Delete a rule */
		case 'C': /* Change counters */
		case 'P': /* Define policy */
		case 'I': /* Insert a rule */
		case 'N': /* Make a user defined chain */
		case 'E': /* Rename chain */
		case 'X': /* Delete chain */
			/* We allow -N chainname -P policy */
			/* XXX: Not in ebtables-compat */
			if (command == 'N' && c == 'P') {
				command = c;
				optind--; /* No table specified */
				break;
			}
			if (OPT_COMMANDS)
				xtables_error(PARAMETER_PROBLEM,
					      "Multiple commands are not allowed");
			command = c;
			chain = optarg;
			selected_chain = get_current_chain(chain);
			p.chain = chain;
			flags |= OPT_COMMAND;

			if (c == 'N') {
				printf("add chain bridge %s %s\n", p.table, p.chain);
				ret = 1;
				break;
			} else if (c == 'X') {
				printf("delete chain bridge %s %s\n", p.table, p.chain);
				ret = 1;
				break;
			}

			if (c == 'E') {
				break;
			} else if (c == 'D' && optind < argc && (argv[optind][0] != '-' || (argv[optind][1] >= '0' && argv[optind][1] <= '9'))) {
				if (optind != argc - 1)
					xtables_error(PARAMETER_PROBLEM,
							 "No extra options allowed with -D start_nr[:end_nr]");
				if (parse_rule_range(argv[optind], &rule_nr, &rule_nr_end))
					xtables_error(PARAMETER_PROBLEM,
							 "Problem with the specified rule number(s) '%s'", argv[optind]);
				optind++;
			} else if (c == 'C') {
				if ((chcounter = parse_change_counters_rule(argc, argv, &rule_nr, &rule_nr_end, exec_style, &cs)) == -1)
					return -1;
			} else if (c == 'I') {
				if (optind >= argc || (argv[optind][0] == '-' && (argv[optind][1] < '0' || argv[optind][1] > '9')))
					rule_nr = 1;
				else {
					rule_nr = parse_rule_number(argv[optind]);
					optind++;
				}
				p.rulenum = rule_nr;
			} else if (c == 'P') {
				break;
			}
			break;
		case 'L': /* List */
			printf("list table bridge %s\n", p.table);
			ret = 1;
			break;
		case 'F': /* Flush */
			if (p.chain) {
				printf("flush chain bridge %s %s\n", p.table, p.chain);
			} else {
				printf("flush table bridge %s\n", p.table);
			}
			ret = 1;
			break;
		case 'Z': /* Zero counters */
			if (c == 'Z') {
				if ((flags & OPT_ZERO) || (flags & OPT_COMMAND && command != 'L'))
print_zero:
					xtables_error(PARAMETER_PROBLEM,
						      "Command -Z only allowed together with command -L");
				flags |= OPT_ZERO;
			} else {
				if (flags & OPT_COMMAND)
					xtables_error(PARAMETER_PROBLEM,
						      "Multiple commands are not allowed");
				command = c;
				flags |= OPT_COMMAND;
				if (flags & OPT_ZERO && c != 'L')
					goto print_zero;
			}
			break;
		case 'V': /* Version */
			if (OPT_COMMANDS)
				xtables_error(PARAMETER_PROBLEM,
					      "Multiple commands are not allowed");
			command = 'V';
			if (exec_style == EXEC_STYLE_DAEMON)
				xtables_error(PARAMETER_PROBLEM,
					      "%s %s\n", prog_name, prog_vers);
			printf("%s %s\n", prog_name, prog_vers);
			exit(0);
		case 'h':
			if (OPT_COMMANDS)
				xtables_error(PARAMETER_PROBLEM,
					      "Multiple commands are not allowed");
			print_help();
			break;
		case 't': /* Table */
			if (OPT_COMMANDS)
				xtables_error(PARAMETER_PROBLEM,
					      "Please put the -t option first");
			ebt_check_option2(&flags, OPT_TABLE);
			if (strlen(optarg) > EBT_TABLE_MAXNAMELEN - 1)
				xtables_error(PARAMETER_PROBLEM,
					      "Table name length cannot exceed %d characters",
					      EBT_TABLE_MAXNAMELEN - 1);
			*table = optarg;
			break;
		case 'i': /* Input interface */
		case 2  : /* Logical input interface */
		case 'o': /* Output interface */
		case 3  : /* Logical output interface */
		case 'j': /* Target */
		case 'p': /* Net family protocol */
		case 's': /* Source mac */
		case 'd': /* Destination mac */
		case 'c': /* Set counters */
			if (!OPT_COMMANDS)
				xtables_error(PARAMETER_PROBLEM,
					      "No command specified");
			if (command != 'A' && command != 'D' && command != 'I' && command != 'C')
				xtables_error(PARAMETER_PROBLEM,
					      "Command and option do not match");
			if (c == 'i') {
				ebt_check_option2(&flags, OPT_IN);
				if (selected_chain > 2 && selected_chain < NF_BR_BROUTING)
					xtables_error(PARAMETER_PROBLEM,
						      "Use -i only in INPUT, FORWARD, PREROUTING and BROUTING chains");
				if (ebt_check_inverse2(optarg, argc, argv))
					cs.fw.invflags |= EBT_IIN;

				if (strlen(optarg) >= IFNAMSIZ)
big_iface_length:
					xtables_error(PARAMETER_PROBLEM,
						      "Interface name length cannot exceed %d characters",
						      IFNAMSIZ - 1);
				xtables_parse_interface(optarg, cs.fw.in, cs.fw.in_mask);
				break;
			} else if (c == 2) {
				ebt_check_option2(&flags, OPT_LOGICALIN);
				if (selected_chain > 2 && selected_chain < NF_BR_BROUTING)
					xtables_error(PARAMETER_PROBLEM,
						      "Use --logical-in only in INPUT, FORWARD, PREROUTING and BROUTING chains");
				if (ebt_check_inverse2(optarg, argc, argv))
					cs.fw.invflags |= EBT_ILOGICALIN;

				if (strlen(optarg) >= IFNAMSIZ)
					goto big_iface_length;
				strcpy(cs.fw.logical_in, optarg);
				if (parse_iface(cs.fw.logical_in, "--logical-in"))
					return -1;
				break;
			} else if (c == 'o') {
				ebt_check_option2(&flags, OPT_OUT);
				if (selected_chain < 2 || selected_chain == NF_BR_BROUTING)
					xtables_error(PARAMETER_PROBLEM,
						      "Use -o only in OUTPUT, FORWARD and POSTROUTING chains");
				if (ebt_check_inverse2(optarg, argc, argv))
					cs.fw.invflags |= EBT_IOUT;

				if (strlen(optarg) >= IFNAMSIZ)
					goto big_iface_length;

				xtables_parse_interface(optarg, cs.fw.out, cs.fw.out_mask);
				break;
			} else if (c == 3) {
				ebt_check_option2(&flags, OPT_LOGICALOUT);
				if (selected_chain < 2 || selected_chain == NF_BR_BROUTING)
					xtables_error(PARAMETER_PROBLEM,
						      "Use --logical-out only in OUTPUT, FORWARD and POSTROUTING chains");
				if (ebt_check_inverse2(optarg, argc, argv))
					cs.fw.invflags |= EBT_ILOGICALOUT;

				if (strlen(optarg) >= IFNAMSIZ)
					goto big_iface_length;
				strcpy(cs.fw.logical_out, optarg);
				if (parse_iface(cs.fw.logical_out, "--logical-out"))
					return -1;
				break;
			} else if (c == 'j') {
				ebt_check_option2(&flags, OPT_JUMP);
				cs.jumpto = parse_target(optarg);
				cs.target = command_jump(&cs, cs.jumpto);
				break;
			} else if (c == 's') {
				ebt_check_option2(&flags, OPT_SOURCE);
				if (ebt_check_inverse2(optarg, argc, argv))
					cs.fw.invflags |= EBT_ISOURCE;

				if (ebt_get_mac_and_mask(optarg, cs.fw.sourcemac, cs.fw.sourcemsk))
					xtables_error(PARAMETER_PROBLEM, "Problem with specified source mac '%s'", optarg);
				cs.fw.bitmask |= EBT_SOURCEMAC;
				break;
			} else if (c == 'd') {
				ebt_check_option2(&flags, OPT_DEST);
				if (ebt_check_inverse2(optarg, argc, argv))
					cs.fw.invflags |= EBT_IDEST;

				if (ebt_get_mac_and_mask(optarg, cs.fw.destmac, cs.fw.destmsk))
					xtables_error(PARAMETER_PROBLEM, "Problem with specified destination mac '%s'", optarg);
				cs.fw.bitmask |= EBT_DESTMAC;
				break;
			} else if (c == 'c') {
				ebt_check_option2(&flags, OPT_COUNT);
				if (ebt_check_inverse2(optarg, argc, argv))
					xtables_error(PARAMETER_PROBLEM,
						      "Unexpected '!' after -c");
				if (optind >= argc || optarg[0] == '-' || argv[optind][0] == '-')
					xtables_error(PARAMETER_PROBLEM,
						      "Option -c needs 2 arguments");

				cs.counters.pcnt = strtoull(optarg, &buffer, 10);
				if (*buffer != '\0')
					xtables_error(PARAMETER_PROBLEM,
						      "Packet counter '%s' invalid",
						      optarg);
				cs.counters.bcnt = strtoull(argv[optind], &buffer, 10);
				if (*buffer != '\0')
					xtables_error(PARAMETER_PROBLEM,
						      "Packet counter '%s' invalid",
						      argv[optind]);
				optind++;
				break;
			}
			ebt_check_option2(&flags, OPT_PROTOCOL);
			if (ebt_check_inverse2(optarg, argc, argv))
				cs.fw.invflags |= EBT_IPROTO;

			cs.fw.bitmask &= ~((unsigned int)EBT_NOPROTO);
			i = strtol(optarg, &buffer, 16);
			if (*buffer == '\0' && (i < 0 || i > 0xFFFF))
				xtables_error(PARAMETER_PROBLEM,
					      "Problem with the specified protocol");
			if (*buffer != '\0') {
				struct ethertypeent *ent;

				if (!strcasecmp(optarg, "LENGTH")) {
					cs.fw.bitmask |= EBT_802_3;
					break;
				}
				ent = getethertypebyname(optarg);
				if (!ent)
					xtables_error(PARAMETER_PROBLEM,
						      "Problem with the specified Ethernet protocol '%s', perhaps "_PATH_ETHERTYPES " is missing", optarg);
				cs.fw.ethproto = ent->e_ethertype;
			} else
				cs.fw.ethproto = i;

			if (cs.fw.ethproto < 0x0600)
				xtables_error(PARAMETER_PROBLEM,
					      "Sorry, protocols have values above or equal to 0x0600");
			break;
		case 4  : /* Lc */
			ebt_check_option2(&flags, LIST_C);
			if (command != 'L')
				xtables_error(PARAMETER_PROBLEM,
					      "Use --Lc with -L");
			flags |= LIST_C;
			break;
		case 5  : /* Ln */
			ebt_check_option2(&flags, LIST_N);
			if (command != 'L')
				xtables_error(PARAMETER_PROBLEM,
					      "Use --Ln with -L");
			if (flags & LIST_X)
				xtables_error(PARAMETER_PROBLEM,
					      "--Lx is not compatible with --Ln");
			flags |= LIST_N;
			break;
		case 6  : /* Lx */
			ebt_check_option2(&flags, LIST_X);
			if (command != 'L')
				xtables_error(PARAMETER_PROBLEM,
					      "Use --Lx with -L");
			if (flags & LIST_N)
				xtables_error(PARAMETER_PROBLEM,
					      "--Lx is not compatible with --Ln");
			flags |= LIST_X;
			break;
		case 12 : /* Lmac2 */
			ebt_check_option2(&flags, LIST_MAC2);
			if (command != 'L')
				xtables_error(PARAMETER_PROBLEM,
					       "Use --Lmac2 with -L");
			flags |= LIST_MAC2;
			break;
		case 1 :
			if (!strcmp(optarg, "!"))
				ebt_check_inverse2(optarg, argc, argv);
			else
				xtables_error(PARAMETER_PROBLEM,
					      "Bad argument : '%s'", optarg);
			/* ebt_ebt_check_inverse2() did optind++ */
			optind--;
			continue;
		default:
			/* Is it a target option? */
			if (cs.target != NULL && cs.target->parse != NULL) {
				int opt_offset = cs.target->option_offset;
				if (cs.target->parse(c - opt_offset,
						     argv, ebt_invert,
						     &cs.target->tflags,
						     NULL, &cs.target->t))
					goto check_extension;
			}

			/* Is it a match_option? */
			for (m = xtables_matches; m; m = m->next) {
				if (m->parse(c - m->option_offset, argv, ebt_check_inverse2(optarg, argc, argv), &m->mflags, NULL, &m->m)) {
					ebt_add_match(m, &cs);
					goto check_extension;
				}
			}

			/* Is it a watcher option? */
			for (w = xtables_targets; w; w = w->next) {
				if (w->parse(c - w->option_offset, argv,
					     ebt_invert, &w->tflags,
					     NULL, &w->t)) {
					ebt_add_watcher(w, &cs);
					goto check_extension;
				}
			}
check_extension:
			if (command != 'A' && command != 'I' &&
			    command != 'D' && command != 'C')
				xtables_error(PARAMETER_PROBLEM,
					      "Extensions only for -A, -I, -D and -C");
		}
		ebt_invert = 0;
	}

	/* Do the final checks */
	if (command == 'A' || command == 'I' ||
	    command == 'D' || command == 'C') {
		for (xtrm_i = cs.matches; xtrm_i; xtrm_i = xtrm_i->next)
			xtables_option_mfcall(xtrm_i->match);

		for (match = cs.match_list; match; match = match->next) {
			if (match->ismatch)
				continue;

			xtables_option_tfcall(match->u.watcher);
		}

		if (cs.target != NULL)
			xtables_option_tfcall(cs.target);
	}

	cs.fw.ethproto = htons(cs.fw.ethproto);

	if (command == 'P') {
		return 0;
	} else if (command == 'A') {
		ret = nft_rule_eb_xlate_add(h, &p, &cs, true);
		if (!ret)
			print_ebt_cmd(argc, argv);
	} else if (command == 'I') {
		ret = nft_rule_eb_xlate_add(h, &p, &cs, false);
		if (!ret)
			print_ebt_cmd(argc, argv);
	}

	ebt_cs_clean(&cs);
	return ret;
}

int xtables_eb_xlate_main(int argc, char *argv[])
{
	int ret;
	char *table = "filter";
	struct nft_handle h = {
		.family = NFPROTO_BRIDGE,
	};

	ebtables_globals.program_name = argv[0];
	ret = xtables_init_all(&ebtables_globals, NFPROTO_BRIDGE);
	if (ret < 0) {
		fprintf(stderr, "%s/%s Failed to initialize xtables\n",
			ebtables_globals.program_name,
			ebtables_globals.program_version);
		exit(EXIT_FAILURE);
	}

	ret = do_commandeb_xlate(&h, argc, argv, &table);
	if (!ret)
		fprintf(stderr, "Translation not implemented\n");

	exit(!ret);
}
