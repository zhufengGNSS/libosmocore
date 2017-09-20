/*
   $Id: command.c,v 1.47 2005/04/25 16:26:42 paul Exp $

   Command interpreter routine for virtual terminal [aka TeletYpe]
   Copyright (C) 1997, 98, 99 Kunihiro Ishiguro

This file is part of GNU Zebra.

GNU Zebra is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published
by the Free Software Foundation; either version 2, or (at your
option) any later version.

GNU Zebra is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Zebra; see the file COPYING.  If not, write to the
Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
Boston, MA  02110-1301, USA. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <syslog.h>
#include <errno.h>
#define _XOPEN_SOURCE
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <osmocom/vty/vector.h>
#include <osmocom/vty/vty.h>
#include <osmocom/vty/command.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>

/*! \addtogroup command
 *  @{
 *  VTY command handling
 *
 * \file command.c */

#define CONFIGFILE_MASK 022

void print_str(const char *str)
{
	if (!str) {
		printf("NULL\n");
		return;
	}
	printf("'");
	while (str && *str) {
		if (*str == '\t') printf("\\t");
		else
		if (*str == '\n') printf("\\n");
		else
			printf("%c", *str);
		str++;
	}
	printf("'\n");
}


void *tall_vty_cmd_ctx;

/* Command vector which includes some level of command lists. Normally
   each daemon maintains each own cmdvec. */
vector cmdvec;

/* Host information structure. */
struct host host;

/* Standard command node structures. */
struct cmd_node auth_node = {
	AUTH_NODE,
	"Password: ",
};

struct cmd_node view_node = {
	VIEW_NODE,
	"%s> ",
};

struct cmd_node auth_enable_node = {
	AUTH_ENABLE_NODE,
	"Password: ",
};

struct cmd_node enable_node = {
	ENABLE_NODE,
	"%s# ",
};

struct cmd_node config_node = {
	CONFIG_NODE,
	"%s(config)# ",
	1
};

/* Default motd string. */
const char *default_motd = "";

/*! print the version (and optionally copyright) information
 *
 * This is called from main when a daemon is invoked with -v or --version. */
void print_version(int print_copyright)
{
	printf("%s version %s\n", host.app_info->name, host.app_info->version);
	if (print_copyright)
		printf("\n%s\n", host.app_info->copyright);
}

/* Utility function to concatenate argv argument into a single string
   with inserting ' ' character between each argument.  */
char *argv_concat(const char **argv, int argc, int shift)
{
	int i;
	size_t len;
	char *str;
	char *p;

	len = 0;
	for (i = shift; i < argc; i++)
		len += strlen(argv[i]) + 1;
	if (!len)
		return NULL;
	p = str = _talloc_zero(tall_vty_cmd_ctx, len, "arvg_concat");
	for (i = shift; i < argc; i++) {
		size_t arglen;
		memcpy(p, argv[i], (arglen = strlen(argv[i])));
		p += arglen;
		*p++ = ' ';
	}
	*(p - 1) = '\0';
	return str;
}

/* Strip all characters from a string (prompt) except for alnum, '-' and '_'.
 * For example used to derive a node->name from node->prompt if the user didn't provide a name;
 * in turn, this name us used for XML IDs in 'show online-help'. */
static const char *node_name_from_prompt(const char *prompt, char *name_buf, size_t name_buf_size)
{
	const char *pos;
	int dest = 0;

	if (!prompt || !*prompt)
		return "";

	for (pos = prompt; *pos && dest < (name_buf_size-1); pos++) {
		if (pos[0] == '%' && pos[1]) {
			/* skip "%s"; loop pos++ does the second one. */
			pos++;
			continue;
		}
		if (!(isalnum(pos[0]) || pos[0] == '-' || pos[0] == '_'))
			continue;
		name_buf[dest] = pos[0];
		dest++;
	}
	name_buf[dest] = '\0';
	return name_buf;
}

/*! Install top node of command vector. */
void install_node(struct cmd_node *node, int (*func) (struct vty *))
{
	vector_set_index(cmdvec, node->node, node);
	node->func = func;
	node->cmd_vector = vector_init(VECTOR_MIN_SIZE);
	if (!*node->name)
		node_name_from_prompt(node->prompt, node->name, sizeof(node->name));
}

/* Compare two command's string.  Used in sort_node (). */
static int cmp_node(const void *p, const void *q)
{
	struct cmd_element *a = *(struct cmd_element **)p;
	struct cmd_element *b = *(struct cmd_element **)q;

	return strcmp(a->string, b->string);
}

static int cmp_desc(const void *p, const void *q)
{
	struct desc *a = *(struct desc **)p;
	struct desc *b = *(struct desc **)q;

	return strcmp(a->cmd, b->cmd);
}

static int is_config_child(struct vty *vty)
{
	if (vty->node <= CONFIG_NODE)
		return 0;
	else if (vty->node > CONFIG_NODE && vty->node < _LAST_OSMOVTY_NODE)
		return 1;
	else if (host.app_info->is_config_node)
		return host.app_info->is_config_node(vty, vty->node);
	else
		return vty->node > CONFIG_NODE;
}

/*! Sort each node's command element according to command string. */
void sort_node(void)
{
	unsigned int i, j;
	struct cmd_node *cnode;
	vector descvec;
	struct cmd_element *cmd_element;

	for (i = 0; i < vector_active(cmdvec); i++)
		if ((cnode = vector_slot(cmdvec, i)) != NULL) {
			vector cmd_vector = cnode->cmd_vector;
			qsort(cmd_vector->index, vector_active(cmd_vector),
			      sizeof(void *), cmp_node);

			for (j = 0; j < vector_active(cmd_vector); j++)
				if ((cmd_element =
				     vector_slot(cmd_vector, j)) != NULL
				    && vector_active(cmd_element->strvec)) {
					descvec =
					    vector_slot(cmd_element->strvec,
							vector_active
							(cmd_element->strvec) -
							1);
					qsort(descvec->index,
					      vector_active(descvec),
					      sizeof(void *), cmp_desc);
				}
		}
}

/*! Break up string in command tokens. Return leading indents.
 * \param[in] string  String to split.
 * \param[out] indent  If not NULL, return a talloc_strdup of indent characters.
 * \param[out] strvec_p  Returns vector of split tokens, must not be NULL.
 * \returns CMD_SUCCESS or CMD_ERR_INVALID_INDENT
 *
 * If \a indent is passed non-NULL, only simple space ' ' indents are allowed,
 * so that \a indent can simply return the count of leading spaces.
 * Otherwise any isspace() characters are allowed for indenting (backwards compat).
 */
int cmd_make_strvec2(const char *string, char **indent, vector *strvec_p)
{
	const char *cp, *start;
	char *token;
	int strlen;
	vector strvec;

	*strvec_p = NULL;
	if (indent)
		*indent = 0;

	if (string == NULL)
		return CMD_SUCCESS;

	cp = string;

	/* Skip white spaces. */
	while (isspace((int)*cp) && *cp != '\0') {
		/* if we're counting indents, we need to be strict about them */
		if (indent && (*cp != ' ') && (*cp != '\t')) {
			/* Ignore blank lines, they appear as leading whitespace with line breaks. */
			if (*cp == '\n' || *cp == '\r') {
				cp++;
				string = cp;
				continue;
			}
			return CMD_ERR_INVALID_INDENT;
		}
		cp++;
	}

	if (indent)
		*indent = talloc_strndup(tall_vty_cmd_ctx, string, cp - string);

	/* Return if there is only white spaces */
	if (*cp == '\0')
		return CMD_SUCCESS;

	if (*cp == '!' || *cp == '#')
		return CMD_SUCCESS;

	/* Prepare return vector. */
	strvec = vector_init(VECTOR_MIN_SIZE);

	/* Copy each command piece and set into vector. */
	while (1) {
		start = cp;
		while (!(isspace((int)*cp) || *cp == '\r' || *cp == '\n') &&
		       *cp != '\0')
			cp++;
		strlen = cp - start;
		token = _talloc_zero(tall_vty_cmd_ctx, strlen + 1, "make_strvec");
		memcpy(token, start, strlen);
		*(token + strlen) = '\0';
		vector_set(strvec, token);

		while ((isspace((int)*cp) || *cp == '\n' || *cp == '\r') &&
		       *cp != '\0')
			cp++;

		if (*cp == '\0')
			break;
	}

	*strvec_p = strvec;
	return CMD_SUCCESS;
}

/*! Breaking up string into each command piece. I assume given
   character is separated by a space character. Return value is a
   vector which includes char ** data element. */
vector cmd_make_strvec(const char *string)
{
	vector strvec;
	cmd_make_strvec2(string, NULL, &strvec);
	return strvec;
}

/*! Free allocated string vector. */
void cmd_free_strvec(vector v)
{
	unsigned int i;
	char *cp;

	if (!v)
		return;

	for (i = 0; i < vector_active(v); i++)
		if ((cp = vector_slot(v, i)) != NULL)
			talloc_free(cp);

	vector_free(v);
}

/*! Fetch next description.  Used in \ref cmd_make_descvec(). */
static char *cmd_desc_str(const char **string)
{
	const char *cp, *start;
	char *token;
	int strlen;

	cp = *string;

	if (cp == NULL)
		return NULL;

	/* Skip white spaces. */
	while (isspace((int)*cp) && *cp != '\0')
		cp++;

	/* Return if there is only white spaces */
	if (*cp == '\0')
		return NULL;

	start = cp;

	while (!(*cp == '\r' || *cp == '\n') && *cp != '\0')
		cp++;

	strlen = cp - start;
	token = _talloc_zero(tall_vty_cmd_ctx, strlen + 1, "cmd_desc_str");
	memcpy(token, start, strlen);
	*(token + strlen) = '\0';

	*string = cp;

	return token;
}

/*! New string vector. */
static vector cmd_make_descvec(const char *string, const char *descstr)
{
	int multiple = 0;
	const char *sp;
	char *token;
	int len;
	const char *cp;
	const char *dp;
	vector allvec;
	vector strvec = NULL;
	struct desc *desc;

	cp = string;
	dp = descstr;

	if (cp == NULL)
		return NULL;

	allvec = vector_init(VECTOR_MIN_SIZE);

	while (1) {
		while (isspace((int)*cp) && *cp != '\0')
			cp++;

		if (*cp == '(') {
			multiple = 1;
			cp++;
		}
		if (*cp == ')') {
			multiple = 0;
			cp++;
		}
		if (*cp == '|') {
			OSMO_ASSERT(multiple);
			cp++;
		}

		while (isspace((int)*cp) && *cp != '\0')
			cp++;

		if (*cp == '(') {
			multiple = 1;
			cp++;
		}

		if (*cp == '\0')
			return allvec;

		sp = cp;

		while (!
		       (isspace((int)*cp) || *cp == '\r' || *cp == '\n'
			|| *cp == ')' || *cp == '|') && *cp != '\0')
			cp++;

		len = cp - sp;

		token = _talloc_zero(tall_vty_cmd_ctx, len + 1, "cmd_make_descvec");
		memcpy(token, sp, len);
		*(token + len) = '\0';

		desc = talloc_zero(tall_vty_cmd_ctx, struct desc);
		desc->cmd = token;
		desc->str = cmd_desc_str(&dp);

		if (multiple) {
			if (multiple == 1) {
				strvec = vector_init(VECTOR_MIN_SIZE);
				vector_set(allvec, strvec);
			}
			multiple++;
		} else {
			strvec = vector_init(VECTOR_MIN_SIZE);
			vector_set(allvec, strvec);
		}
		vector_set(strvec, desc);
	}
}

/* Count mandantory string vector size.  This is to determine inputed
   command has enough command length. */
static int cmd_cmdsize(vector strvec)
{
	unsigned int i;
	int size = 0;
	vector descvec;
	struct desc *desc;

	for (i = 0; i < vector_active(strvec); i++)
		if ((descvec = vector_slot(strvec, i)) != NULL) {
			if ((vector_active(descvec)) == 1
			    && (desc = vector_slot(descvec, 0)) != NULL) {
				if (desc->cmd == NULL || CMD_OPTION(desc->cmd))
					return size;
				else
					size++;
			} else
				size++;
		}
	return size;
}

/*! Return prompt character of specified node. */
const char *cmd_prompt(enum node_type node)
{
	struct cmd_node *cnode;

	cnode = vector_slot(cmdvec, node);
	return cnode->prompt;
}

/*!
 * escape all special asciidoc symbols
 * \param unsafe string
 * \return a new talloc char *
 */
char *osmo_asciidoc_escape(const char *inp)
{
	int _strlen;
	char *out, *out_ptr;
	int len = 0, i, j;

	if (!inp)
		return NULL;
	_strlen = strlen(inp);

	for (i = 0; i < _strlen; ++i) {
		switch (inp[i]) {
		case '|':
			len += 2;
			break;
		default:
			len += 1;
			break;
		}
	}

	out = talloc_size(NULL, len + 1);
	if (!out)
		return NULL;

	out_ptr = out;

#define ADD(out, str) \
	for (j = 0; j < strlen(str); ++j) \
		*(out++) = str[j];

	for (i = 0; i < _strlen; ++i) {
		switch (inp[i]) {
		case '|':
			ADD(out_ptr, "\\|");
			break;
		default:
			*(out_ptr++) = inp[i];
			break;
		}
	}

#undef ADD

	out_ptr[0] = '\0';
	return out;
}

static char *xml_escape(const char *inp)
{
	int _strlen;
	char *out, *out_ptr;
	int len = 0, i, j;

	if (!inp)
		return NULL;
	_strlen = strlen(inp);

	for (i = 0; i < _strlen; ++i) {
		switch (inp[i]) {
		case '"':
			len += 6;
			break;
		case '\'':
			len += 6;
			break;
		case '<':
			len += 4;
			break;
		case '>':
			len += 4;
			break;
		case '&':
			len += 5;
			break;
		default:
			len += 1;
			break;
		}
	}

	out = talloc_size(NULL, len + 1);
	if (!out)
		return NULL;

	out_ptr = out;

#define ADD(out, str) \
	for (j = 0; j < strlen(str); ++j) \
		*(out++) = str[j];

	for (i = 0; i < _strlen; ++i) {
		switch (inp[i]) {
		case '"':
			ADD(out_ptr, "&quot;");
			break;
		case '\'':
			ADD(out_ptr, "&apos;");
			break;
		case '<':
			ADD(out_ptr, "&lt;");
			break;
		case '>':
			ADD(out_ptr, "&gt;");
			break;
		case '&':
			ADD(out_ptr, "&amp;");
			break;
		default:
			*(out_ptr++) = inp[i];
			break;
		}
	}

#undef ADD

	out_ptr[0] = '\0';
	return out;
}

/*
 * Write one cmd_element as XML to the given VTY.
 */
static int vty_dump_element(struct cmd_element *cmd, struct vty *vty)
{
	char *xml_string = xml_escape(cmd->string);

	vty_out(vty, "    <command id='%s'>%s", xml_string, VTY_NEWLINE);
	vty_out(vty, "      <params>%s", VTY_NEWLINE);

	int j;
	for (j = 0; j < vector_count(cmd->strvec); ++j) {
		vector descvec = vector_slot(cmd->strvec, j);
		int i;
		for (i = 0; i < vector_active(descvec); ++i) {
			char *xml_param, *xml_doc;
			struct desc *desc = vector_slot(descvec, i);
			if (desc == NULL)
				continue;

			xml_param = xml_escape(desc->cmd);
			xml_doc = xml_escape(desc->str);
			vty_out(vty, "        <param name='%s' doc='%s' />%s",
				xml_param, xml_doc, VTY_NEWLINE);
			talloc_free(xml_param);
			talloc_free(xml_doc);
		}
	}

	vty_out(vty, "      </params>%s", VTY_NEWLINE);
	vty_out(vty, "    </command>%s", VTY_NEWLINE);

	talloc_free(xml_string);
	return 0;
}

/*
 * Dump all nodes and commands associated with a given node as XML to the VTY.
 */
static int vty_dump_nodes(struct vty *vty)
{
	int i, j;
	int same_name_count;

	vty_out(vty, "<vtydoc xmlns='urn:osmocom:xml:libosmocore:vty:doc:1.0'>%s", VTY_NEWLINE);

	for (i = 0; i < vector_active(cmdvec); ++i) {
		struct cmd_node *cnode;
		cnode = vector_slot(cmdvec, i);
		if (!cnode)
			continue;

		/* De-dup node IDs: how many times has this same name been used before? Count the first
		 * occurence as _1 and omit that first suffix, so that the first occurence is called
		 * 'name', the second becomes 'name_2', then 'name_3', ... */
		same_name_count = 1;
		for (j = 0; j < i; ++j) {
			struct cmd_node *cnode2;
			cnode2 = vector_slot(cmdvec, j);
			if (!cnode2)
				continue;
			if (strcmp(cnode->name, cnode2->name) == 0)
				same_name_count ++;
		}

		vty_out(vty, "  <node id='%s", cnode->name);
		if (same_name_count > 1 || !*cnode->name)
			vty_out(vty, "_%d", same_name_count);
		vty_out(vty, "'>%s", VTY_NEWLINE);

		for (j = 0; j < vector_active(cnode->cmd_vector); ++j) {
			struct cmd_element *elem;
			elem = vector_slot(cnode->cmd_vector, j);
			vty_dump_element(elem, vty);
		}

		vty_out(vty, "  </node>%s", VTY_NEWLINE);
	}

	vty_out(vty, "</vtydoc>%s", VTY_NEWLINE);

	return 0;
}

/* Check if a command with given string exists at given node */
static int check_element_exists(struct cmd_node *cnode, const char *cmdstring)
{
	int i;

	for (i = 0; i < vector_active(cnode->cmd_vector); ++i) {
		struct cmd_element *elem;
		elem = vector_slot(cnode->cmd_vector, i);
		if (!elem->string)
			continue;
		if (!strcmp(elem->string, cmdstring))
			return 1;
	}
	return 0;
}

/*! Install a command into a node
 *  \param[in] ntype Node Type
 *  \param[cmd] element to be installed
 */
void install_element(int ntype, struct cmd_element *cmd)
{
	struct cmd_node *cnode;

	cnode = vector_slot(cmdvec, ntype);

	OSMO_ASSERT(cnode);
	/* ensure no _identical_ command has been registered at this
	 * node so far */
	OSMO_ASSERT(!check_element_exists(cnode, cmd->string));

	vector_set(cnode->cmd_vector, cmd);

	cmd->strvec = cmd_make_descvec(cmd->string, cmd->doc);
	cmd->cmdsize = cmd_cmdsize(cmd->strvec);
}

/* Install a command into VIEW and ENABLE node */
void install_element_ve(struct cmd_element *cmd)
{
	install_element(VIEW_NODE, cmd);
	install_element(ENABLE_NODE, cmd);
}

#ifdef VTY_CRYPT_PW
static unsigned char itoa64[] =
    "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static void to64(char *s, long v, int n)
{
	while (--n >= 0) {
		*s++ = itoa64[v & 0x3f];
		v >>= 6;
	}
}

static char *zencrypt(const char *passwd)
{
	char salt[6];
	struct timeval tv;
	char *crypt(const char *, const char *);

	osmo_gettimeofday(&tv, 0);

	to64(&salt[0], random(), 3);
	to64(&salt[3], tv.tv_usec, 3);
	salt[5] = '\0';

	return crypt(passwd, salt);
}
#endif

/* This function write configuration of this host. */
static int config_write_host(struct vty *vty)
{
	if (host.name)
		vty_out(vty, "hostname %s%s", host.name, VTY_NEWLINE);

	if (host.encrypt) {
		if (host.password_encrypt)
			vty_out(vty, "password 8 %s%s", host.password_encrypt,
				VTY_NEWLINE);
		if (host.enable_encrypt)
			vty_out(vty, "enable password 8 %s%s",
				host.enable_encrypt, VTY_NEWLINE);
	} else {
		if (host.password)
			vty_out(vty, "password %s%s", host.password,
				VTY_NEWLINE);
		if (host.enable)
			vty_out(vty, "enable password %s%s", host.enable,
				VTY_NEWLINE);
	}

	if (host.advanced)
		vty_out(vty, "service advanced-vty%s", VTY_NEWLINE);

	if (host.encrypt)
		vty_out(vty, "service password-encryption%s", VTY_NEWLINE);

	if (host.lines >= 0)
		vty_out(vty, "service terminal-length %d%s", host.lines,
			VTY_NEWLINE);

	if (host.motdfile)
		vty_out(vty, "banner motd file %s%s", host.motdfile,
			VTY_NEWLINE);
	else if (!host.motd)
		vty_out(vty, "no banner motd%s", VTY_NEWLINE);

	return 1;
}

/* Utility function for getting command vector. */
static vector cmd_node_vector(vector v, enum node_type ntype)
{
	struct cmd_node *cnode = vector_slot(v, ntype);
	return cnode->cmd_vector;
}

/* Completion match types. */
enum match_type {
	no_match = 0,
	any_match,
	extend_match,
	ipv4_prefix_match,
	ipv4_match,
	ipv6_prefix_match,
	ipv6_match,
	range_match,
	vararg_match,
	partly_match,
	exact_match,
};

static enum match_type cmd_ipv4_match(const char *str)
{
	const char *sp;
	int dots = 0, nums = 0;
	char buf[4];

	if (str == NULL)
		return partly_match;

	for (;;) {
		memset(buf, 0, sizeof(buf));
		sp = str;
		while (*str != '\0') {
			if (*str == '.') {
				if (dots >= 3)
					return no_match;

				if (*(str + 1) == '.')
					return no_match;

				if (*(str + 1) == '\0')
					return partly_match;

				dots++;
				break;
			}
			if (!isdigit((int)*str))
				return no_match;

			str++;
		}

		if (str - sp > 3)
			return no_match;

		strncpy(buf, sp, str - sp);
		if (atoi(buf) > 255)
			return no_match;

		nums++;

		if (*str == '\0')
			break;

		str++;
	}

	if (nums < 4)
		return partly_match;

	return exact_match;
}

static enum match_type cmd_ipv4_prefix_match(const char *str)
{
	const char *sp;
	int dots = 0;
	char buf[4];

	if (str == NULL)
		return partly_match;

	for (;;) {
		memset(buf, 0, sizeof(buf));
		sp = str;
		while (*str != '\0' && *str != '/') {
			if (*str == '.') {
				if (dots == 3)
					return no_match;

				if (*(str + 1) == '.' || *(str + 1) == '/')
					return no_match;

				if (*(str + 1) == '\0')
					return partly_match;

				dots++;
				break;
			}

			if (!isdigit((int)*str))
				return no_match;

			str++;
		}

		if (str - sp > 3)
			return no_match;

		strncpy(buf, sp, str - sp);
		if (atoi(buf) > 255)
			return no_match;

		if (dots == 3) {
			if (*str == '/') {
				if (*(str + 1) == '\0')
					return partly_match;

				str++;
				break;
			} else if (*str == '\0')
				return partly_match;
		}

		if (*str == '\0')
			return partly_match;

		str++;
	}

	sp = str;
	while (*str != '\0') {
		if (!isdigit((int)*str))
			return no_match;

		str++;
	}

	if (atoi(sp) > 32)
		return no_match;

	return exact_match;
}

#define IPV6_ADDR_STR		"0123456789abcdefABCDEF:.%"
#define IPV6_PREFIX_STR		"0123456789abcdefABCDEF:.%/"
#define STATE_START		1
#define STATE_COLON		2
#define STATE_DOUBLE		3
#define STATE_ADDR		4
#define STATE_DOT               5
#define STATE_SLASH		6
#define STATE_MASK		7

#ifdef HAVE_IPV6

static enum match_type cmd_ipv6_match(const char *str)
{
	int state = STATE_START;
	int colons = 0, nums = 0, double_colon = 0;
	const char *sp = NULL;
	struct sockaddr_in6 sin6_dummy;
	int ret;

	if (str == NULL)
		return partly_match;

	if (strspn(str, IPV6_ADDR_STR) != strlen(str))
		return no_match;

	/* use inet_pton that has a better support,
	 * for example inet_pton can support the automatic addresses:
	 *  ::1.2.3.4
	 */
	ret = inet_pton(AF_INET6, str, &sin6_dummy.sin6_addr);

	if (ret == 1)
		return exact_match;

	while (*str != '\0') {
		switch (state) {
		case STATE_START:
			if (*str == ':') {
				if (*(str + 1) != ':' && *(str + 1) != '\0')
					return no_match;
				colons--;
				state = STATE_COLON;
			} else {
				sp = str;
				state = STATE_ADDR;
			}

			continue;
		case STATE_COLON:
			colons++;
			if (*(str + 1) == ':')
				state = STATE_DOUBLE;
			else {
				sp = str + 1;
				state = STATE_ADDR;
			}
			break;
		case STATE_DOUBLE:
			if (double_colon)
				return no_match;

			if (*(str + 1) == ':')
				return no_match;
			else {
				if (*(str + 1) != '\0')
					colons++;
				sp = str + 1;
				state = STATE_ADDR;
			}

			double_colon++;
			nums++;
			break;
		case STATE_ADDR:
			if (*(str + 1) == ':' || *(str + 1) == '\0') {
				if (str - sp > 3)
					return no_match;

				nums++;
				state = STATE_COLON;
			}
			if (*(str + 1) == '.')
				state = STATE_DOT;
			break;
		case STATE_DOT:
			state = STATE_ADDR;
			break;
		default:
			break;
		}

		if (nums > 8)
			return no_match;

		if (colons > 7)
			return no_match;

		str++;
	}

#if 0
	if (nums < 11)
		return partly_match;
#endif				/* 0 */

	return exact_match;
}

static enum match_type cmd_ipv6_prefix_match(const char *str)
{
	int state = STATE_START;
	int colons = 0, nums = 0, double_colon = 0;
	int mask;
	const char *sp = NULL;
	char *endptr = NULL;

	if (str == NULL)
		return partly_match;

	if (strspn(str, IPV6_PREFIX_STR) != strlen(str))
		return no_match;

	while (*str != '\0' && state != STATE_MASK) {
		switch (state) {
		case STATE_START:
			if (*str == ':') {
				if (*(str + 1) != ':' && *(str + 1) != '\0')
					return no_match;
				colons--;
				state = STATE_COLON;
			} else {
				sp = str;
				state = STATE_ADDR;
			}

			continue;
		case STATE_COLON:
			colons++;
			if (*(str + 1) == '/')
				return no_match;
			else if (*(str + 1) == ':')
				state = STATE_DOUBLE;
			else {
				sp = str + 1;
				state = STATE_ADDR;
			}
			break;
		case STATE_DOUBLE:
			if (double_colon)
				return no_match;

			if (*(str + 1) == ':')
				return no_match;
			else {
				if (*(str + 1) != '\0' && *(str + 1) != '/')
					colons++;
				sp = str + 1;

				if (*(str + 1) == '/')
					state = STATE_SLASH;
				else
					state = STATE_ADDR;
			}

			double_colon++;
			nums += 1;
			break;
		case STATE_ADDR:
			if (*(str + 1) == ':' || *(str + 1) == '.'
			    || *(str + 1) == '\0' || *(str + 1) == '/') {
				if (str - sp > 3)
					return no_match;

				for (; sp <= str; sp++)
					if (*sp == '/')
						return no_match;

				nums++;

				if (*(str + 1) == ':')
					state = STATE_COLON;
				else if (*(str + 1) == '.')
					state = STATE_DOT;
				else if (*(str + 1) == '/')
					state = STATE_SLASH;
			}
			break;
		case STATE_DOT:
			state = STATE_ADDR;
			break;
		case STATE_SLASH:
			if (*(str + 1) == '\0')
				return partly_match;

			state = STATE_MASK;
			break;
		default:
			break;
		}

		if (nums > 11)
			return no_match;

		if (colons > 7)
			return no_match;

		str++;
	}

	if (state < STATE_MASK)
		return partly_match;

	mask = strtol(str, &endptr, 10);
	if (*endptr != '\0')
		return no_match;

	if (mask < 0 || mask > 128)
		return no_match;

/* I don't know why mask < 13 makes command match partly.
   Forgive me to make this comments. I Want to set static default route
   because of lack of function to originate default in ospf6d; sorry
       yasu
  if (mask < 13)
    return partly_match;
*/

	return exact_match;
}

#endif				/* HAVE_IPV6  */

#define DECIMAL_STRLEN_MAX 10

static int cmd_range_match(const char *range, const char *str)
{
	char *p;
	char buf[DECIMAL_STRLEN_MAX + 1];
	char *endptr = NULL;

	if (str == NULL)
		return 1;

	if (range[1] == '-') {
		signed long min = 0, max = 0, val;

		val = strtol(str, &endptr, 10);
		if (*endptr != '\0')
			return 0;

		range += 2;
		p = strchr(range, '-');
		if (p == NULL)
			return 0;
		if (p - range > DECIMAL_STRLEN_MAX)
			return 0;
		strncpy(buf, range, p - range);
		buf[p - range] = '\0';
		min = -strtol(buf, &endptr, 10);
		if (*endptr != '\0')
			return 0;

		range = p + 1;
		p = strchr(range, '>');
		if (p == NULL)
			return 0;
		if (p - range > DECIMAL_STRLEN_MAX)
			return 0;
		strncpy(buf, range, p - range);
		buf[p - range] = '\0';
		max = strtol(buf, &endptr, 10);
		if (*endptr != '\0')
			return 0;

		if (val < min || val > max)
			return 0;
	} else {
		unsigned long min, max, val;

		val = strtoul(str, &endptr, 10);
		if (*endptr != '\0')
			return 0;

		range++;
		p = strchr(range, '-');
		if (p == NULL)
			return 0;
		if (p - range > DECIMAL_STRLEN_MAX)
			return 0;
		strncpy(buf, range, p - range);
		buf[p - range] = '\0';
		min = strtoul(buf, &endptr, 10);
		if (*endptr != '\0')
			return 0;

		range = p + 1;
		p = strchr(range, '>');
		if (p == NULL)
			return 0;
		if (p - range > DECIMAL_STRLEN_MAX)
			return 0;
		strncpy(buf, range, p - range);
		buf[p - range] = '\0';
		max = strtoul(buf, &endptr, 10);
		if (*endptr != '\0')
			return 0;

		if (val < min || val > max)
			return 0;
	}

	return 1;
}

/* helper to retrieve the 'real' argument string from an optional argument */
static char *
cmd_deopt(const char *str)
{
	/* we've got "[blah]". We want to strip off the []s and redo the
	 * match check for "blah"
	 */
	size_t len = strlen(str);
	char *tmp;

	if (len < 3)
		return NULL;

	/* tmp will hold a string of len-2 chars, so 'len' size is fine */
	tmp = talloc_size(NULL, len);

	memcpy(tmp, (str + 1), len - 2);
	tmp[len - 2] = '\0';

	return tmp;
}

static enum match_type
cmd_match(const char *str, const char *command,
          enum match_type min, bool recur)
{

	if (recur && CMD_OPTION(str))
	{
		enum match_type ret;
		char *tmp = cmd_deopt(str);

		/* this would be a bug in a command, however handle it gracefully
		 * as it we only discover it if a user tries to run it
		 */
		if (tmp == NULL)
			return no_match;

		ret = cmd_match(tmp, command, min, false);

		talloc_free(tmp);

		return ret;
	}
	else if (CMD_VARARG(str))
		return vararg_match;
	else if (CMD_RANGE(str))
	{
		if (cmd_range_match(str, command))
			return range_match;
	}
#ifdef HAVE_IPV6
	else if (CMD_IPV6(str))
	{
		if (cmd_ipv6_match(command) >= min)
			return ipv6_match;
	}
	else if (CMD_IPV6_PREFIX(str))
	{
		if (cmd_ipv6_prefix_match(command) >= min)
			return ipv6_prefix_match;
	}
#endif /* HAVE_IPV6  */
	else if (CMD_IPV4(str))
	{
		if (cmd_ipv4_match(command) >= min)
			return ipv4_match;
	}
	else if (CMD_IPV4_PREFIX(str))
	{
		if (cmd_ipv4_prefix_match(command) >= min)
			return ipv4_prefix_match;
	}
	else if (CMD_VARIABLE(str))
		return extend_match;
	else if (strncmp(command, str, strlen(command)) == 0)
	{
		if (strcmp(command, str) == 0)
			return  exact_match;
		else if (partly_match >= min)
			return partly_match;
	}

	return no_match;
}

/* Filter vector at the specified index and by the given command string, to
 * the desired matching level (thus allowing part matches), and return match
 * type flag.
 */
static enum match_type
cmd_filter(char *command, vector v, unsigned int index, enum match_type level)
{
	unsigned int i;
	struct cmd_element *cmd_element;
	enum match_type match_type;
	vector descvec;
	struct desc *desc;

	match_type = no_match;

	/* If command and cmd_element string does not match set NULL to vector */
	for (i = 0; i < vector_active(v); i++)
		if ((cmd_element = vector_slot(v, i)) != NULL) {
			if (index >= vector_active(cmd_element->strvec))
				vector_slot(v, i) = NULL;
			else {
				unsigned int j;
				int matched = 0;

				descvec =
				    vector_slot(cmd_element->strvec, index);

				for (j = 0; j < vector_active(descvec); j++)
					if ((desc = vector_slot(descvec, j))) {
						enum match_type ret;

						ret = cmd_match (desc->cmd, command, level, true);

						if (ret != no_match)
							matched++;

						if (match_type < ret)
							match_type = ret;
					}
				if (!matched)
					vector_slot(v, i) = NULL;
			}
		}

	if (match_type == no_match)
		return no_match;

	/* 2nd pass: We now know the 'strongest' match type for the index, so we
	 * go again and filter out commands whose argument (at this index) is
	 * 'weaker'. E.g., if we have 2 commands:
	 *
	 *   foo bar <1-255>
	 *   foo bar BLAH
	 *
	 * and the command string is 'foo bar 10', then we will get here with with
	 * 'range_match' being the strongest match.  However, if 'BLAH' came
	 * earlier, it won't have been filtered out (as a CMD_VARIABLE allows "10").
	 *
	 * If we don't do a 2nd pass and filter it out, the higher-layers will
	 * consider this to be ambiguous.
	 */
	for (i = 0; i < vector_active(v); i++)
		if ((cmd_element = vector_slot(v, i)) != NULL) {
			if (index >= vector_active(cmd_element->strvec))
				vector_slot(v, i) = NULL;
			else {
				unsigned int j;
				int matched = 0;

				descvec =
				    vector_slot(cmd_element->strvec, index);

				for (j = 0; j < vector_active(descvec); j++)
					if ((desc = vector_slot(descvec, j))) {
						enum match_type ret;

						ret = cmd_match(desc->cmd, command, any_match, true);

						if (ret >= match_type)
							matched++;
					}
				if (!matched)
					vector_slot(v, i) = NULL;
			}
		}

	return match_type;
}

/* Check ambiguous match */
static int
is_cmd_ambiguous(char *command, vector v, int index, enum match_type type)
{
	unsigned int i;
	unsigned int j;
	struct cmd_element *cmd_element;
	const char *matched = NULL;
	vector descvec;
	struct desc *desc;

	for (i = 0; i < vector_active(v); i++)
		if ((cmd_element = vector_slot(v, i)) != NULL) {
			int match = 0;

			descvec = vector_slot(cmd_element->strvec, index);

			for (j = 0; j < vector_active(descvec); j++)
				if ((desc = vector_slot(descvec, j))) {
					enum match_type ret;
					const char *str = desc->cmd;

					if (CMD_OPTION(str))
						if ((str = cmd_deopt(str)) == NULL)
							continue;

					switch (type) {
					case exact_match:
						if (!(CMD_VARIABLE (str))
						   && strcmp(command, str) == 0)
							match++;
						break;
					case partly_match:
						if (!(CMD_VARIABLE (str))
						   && strncmp(command, str, strlen (command)) == 0)
						{
							if (matched
							    && strcmp(matched,
								      str) != 0)
								return 1;	/* There is ambiguous match. */
							else
								matched = str;
							match++;
						}
						break;
					case range_match:
						if (cmd_range_match
						    (str, command)) {
							if (matched
							    && strcmp(matched,
								      str) != 0)
								return 1;
							else
								matched = str;
							match++;
						}
						break;
#ifdef HAVE_IPV6
					case ipv6_match:
						if (CMD_IPV6(str))
							match++;
						break;
					case ipv6_prefix_match:
						if ((ret =
						     cmd_ipv6_prefix_match
						     (command)) != no_match) {
							if (ret == partly_match)
								return 2;	/* There is incomplete match. */

							match++;
						}
						break;
#endif				/* HAVE_IPV6 */
					case ipv4_match:
						if (CMD_IPV4(str))
							match++;
						break;
					case ipv4_prefix_match:
						if ((ret =
						     cmd_ipv4_prefix_match
						     (command)) != no_match) {
							if (ret == partly_match)
								return 2;	/* There is incomplete match. */

							match++;
						}
						break;
					case extend_match:
						if (CMD_VARIABLE (str))
							match++;
						break;
					case no_match:
					default:
						break;
					}

					if (CMD_OPTION(desc->cmd))
						talloc_free((void*)str);
				}
			if (!match)
				vector_slot(v, i) = NULL;
		}
	return 0;
}

/* If src matches dst return dst string, otherwise return NULL */
static const char *cmd_entry_function(const char *src, const char *dst)
{
	/* Skip variable arguments. */
	if (CMD_OPTION(dst) || CMD_VARIABLE(dst) || CMD_VARARG(dst) ||
	    CMD_IPV4(dst) || CMD_IPV4_PREFIX(dst) || CMD_RANGE(dst))
		return NULL;

	/* In case of 'command \t', given src is NULL string. */
	if (src == NULL)
		return dst;

	/* Matched with input string. */
	if (strncmp(src, dst, strlen(src)) == 0)
		return dst;

	return NULL;
}

/* If src matches dst return dst string, otherwise return NULL */
/* This version will return the dst string always if it is
   CMD_VARIABLE for '?' key processing */
static const char *cmd_entry_function_desc(const char *src, const char *dst)
{
	if (CMD_VARARG(dst))
		return dst;

	if (CMD_RANGE(dst)) {
		if (cmd_range_match(dst, src))
			return dst;
		else
			return NULL;
	}
#ifdef HAVE_IPV6
	if (CMD_IPV6(dst)) {
		if (cmd_ipv6_match(src))
			return dst;
		else
			return NULL;
	}

	if (CMD_IPV6_PREFIX(dst)) {
		if (cmd_ipv6_prefix_match(src))
			return dst;
		else
			return NULL;
	}
#endif				/* HAVE_IPV6 */

	if (CMD_IPV4(dst)) {
		if (cmd_ipv4_match(src))
			return dst;
		else
			return NULL;
	}

	if (CMD_IPV4_PREFIX(dst)) {
		if (cmd_ipv4_prefix_match(src))
			return dst;
		else
			return NULL;
	}

	/* Optional or variable commands always match on '?' */
	if (CMD_OPTION(dst) || CMD_VARIABLE(dst))
		return dst;

	/* In case of 'command \t', given src is NULL string. */
	if (src == NULL)
		return dst;

	if (strncmp(src, dst, strlen(src)) == 0)
		return dst;
	else
		return NULL;
}

/* Check same string element existence.  If it isn't there return
    1. */
static int cmd_unique_string(vector v, const char *str)
{
	unsigned int i;
	char *match;

	for (i = 0; i < vector_active(v); i++)
		if ((match = vector_slot(v, i)) != NULL)
			if (strcmp(match, str) == 0)
				return 0;
	return 1;
}

/* Compare string to description vector.  If there is same string
   return 1 else return 0. */
static int desc_unique_string(vector v, const char *str)
{
	unsigned int i;
	struct desc *desc;

	for (i = 0; i < vector_active(v); i++)
		if ((desc = vector_slot(v, i)) != NULL)
			if (strcmp(desc->cmd, str) == 0)
				return 1;
	return 0;
}

static int cmd_try_do_shortcut(enum node_type node, char *first_word)
{
	if (first_word != NULL &&
	    node != AUTH_NODE &&
	    node != VIEW_NODE &&
	    node != AUTH_ENABLE_NODE &&
	    node != ENABLE_NODE && 0 == strcmp("do", first_word))
		return 1;
	return 0;
}

/* '?' describe command support. */
static vector
cmd_describe_command_real(vector vline, struct vty *vty, int *status)
{
	unsigned int i;
	vector cmd_vector;
#define INIT_MATCHVEC_SIZE 10
	vector matchvec;
	struct cmd_element *cmd_element;
	unsigned int index;
	int ret;
	enum match_type match;
	char *command;
	static struct desc desc_cr = { "<cr>", "" };

	/* Set index. */
	if (vector_active(vline) == 0) {
		*status = CMD_ERR_NO_MATCH;
		return NULL;
	} else
		index = vector_active(vline) - 1;

	/* Make copy vector of current node's command vector. */
	cmd_vector = vector_copy(cmd_node_vector(cmdvec, vty->node));

	/* Prepare match vector */
	matchvec = vector_init(INIT_MATCHVEC_SIZE);

	/* Filter commands. */
	/* Only words precedes current word will be checked in this loop. */
	for (i = 0; i < index; i++) {
		command = vector_slot(vline, i);
		if (!command)
			continue;

		match = cmd_filter(command, cmd_vector, i, any_match);

		if (match == vararg_match) {
			struct cmd_element *cmd_element;
			vector descvec;
			unsigned int j, k;

			for (j = 0; j < vector_active(cmd_vector); j++)
				if ((cmd_element =
				     vector_slot(cmd_vector, j)) != NULL
				    &&
				    (vector_active(cmd_element->strvec))) {
					descvec =
					    vector_slot(cmd_element->
							strvec,
							vector_active
							(cmd_element->
							 strvec) - 1);
					for (k = 0;
					     k < vector_active(descvec);
					     k++) {
						struct desc *desc =
						    vector_slot(descvec,
								k);
						vector_set(matchvec,
							   desc);
					}
				}

			vector_set(matchvec, &desc_cr);
			vector_free(cmd_vector);

			return matchvec;
		}

		if ((ret = is_cmd_ambiguous(command, cmd_vector, i,
					    match)) == 1) {
			vector_free(cmd_vector);
			vector_free(matchvec);
			*status = CMD_ERR_AMBIGUOUS;
			return NULL;
		} else if (ret == 2) {
			vector_free(cmd_vector);
			vector_free(matchvec);
			*status = CMD_ERR_NO_MATCH;
			return NULL;
		}
	}

	/* Prepare match vector */
	/*  matchvec = vector_init (INIT_MATCHVEC_SIZE); */

	/* Make sure that cmd_vector is filtered based on current word */
	command = vector_slot(vline, index);
	if (command)
		cmd_filter(command, cmd_vector, index, any_match);

	/* Make description vector. */
	for (i = 0; i < vector_active(cmd_vector); i++) {
		const char *string = NULL;
		vector strvec;

		cmd_element = vector_slot(cmd_vector, i);
		if (!cmd_element)
			continue;

		if (cmd_element->attr & (CMD_ATTR_DEPRECATED|CMD_ATTR_HIDDEN))
			continue;

		strvec = cmd_element->strvec;

		/* if command is NULL, index may be equal to vector_active */
		if (command && index >= vector_active(strvec))
			vector_slot(cmd_vector, i) = NULL;
		else {
			/* Check if command is completed. */
			if (command == NULL
			    && index == vector_active(strvec)) {
				string = "<cr>";
				if (!desc_unique_string(matchvec, string))
					vector_set(matchvec, &desc_cr);
			} else {
				unsigned int j;
				vector descvec = vector_slot(strvec, index);
				struct desc *desc;

				for (j = 0; j < vector_active(descvec); j++) {
					desc = vector_slot(descvec, j);
					if (!desc)
						continue;
					string = cmd_entry_function_desc
							(command, desc->cmd);
					if (!string)
						continue;
					/* Uniqueness check */
					if (!desc_unique_string(matchvec, string))
						vector_set(matchvec, desc);
				}
			}
		}
	}
	vector_free(cmd_vector);

	if (vector_slot(matchvec, 0) == NULL) {
		vector_free(matchvec);
		*status = CMD_ERR_NO_MATCH;
	} else
		*status = CMD_SUCCESS;

	return matchvec;
}

vector cmd_describe_command(vector vline, struct vty * vty, int *status)
{
	vector ret;

	if (cmd_try_do_shortcut(vty->node, vector_slot(vline, 0))) {
		enum node_type onode;
		vector shifted_vline;
		unsigned int index;

		onode = vty->node;
		vty->node = ENABLE_NODE;
		/* We can try it on enable node, cos' the vty is authenticated */

		shifted_vline = vector_init(vector_count(vline));
		/* use memcpy? */
		for (index = 1; index < vector_active(vline); index++) {
			vector_set_index(shifted_vline, index - 1,
					 vector_lookup(vline, index));
		}

		ret = cmd_describe_command_real(shifted_vline, vty, status);

		vector_free(shifted_vline);
		vty->node = onode;
		return ret;
	}

	return cmd_describe_command_real(vline, vty, status);
}

/* Check LCD of matched command. */
static int cmd_lcd(char **matched)
{
	int i;
	int j;
	int lcd = -1;
	char *s1, *s2;
	char c1, c2;

	if (matched[0] == NULL || matched[1] == NULL)
		return 0;

	for (i = 1; matched[i] != NULL; i++) {
		s1 = matched[i - 1];
		s2 = matched[i];

		for (j = 0; (c1 = s1[j]) && (c2 = s2[j]); j++)
			if (c1 != c2)
				break;

		if (lcd < 0)
			lcd = j;
		else {
			if (lcd > j)
				lcd = j;
		}
	}
	return lcd;
}

/* Command line completion support. */
static char **cmd_complete_command_real(vector vline, struct vty *vty,
					int *status)
{
	unsigned int i;
	vector cmd_vector = vector_copy(cmd_node_vector(cmdvec, vty->node));
#define INIT_MATCHVEC_SIZE 10
	vector matchvec;
	struct cmd_element *cmd_element;
	unsigned int index;
	char **match_str;
	struct desc *desc;
	vector descvec;
	char *command;
	int lcd;

	if (vector_active(vline) == 0) {
		*status = CMD_ERR_NO_MATCH;
		vector_free(cmd_vector);
		return NULL;
	} else
		index = vector_active(vline) - 1;

	/* First, filter by preceeding command string */
	for (i = 0; i < index; i++)
		if ((command = vector_slot(vline, i))) {
			enum match_type match;
			int ret;

			/* First try completion match, if there is exactly match return 1 */
			match =
			    cmd_filter(command, cmd_vector, i, any_match);

			/* If there is exact match then filter ambiguous match else check
			   ambiguousness. */
			if ((ret =
			     is_cmd_ambiguous(command, cmd_vector, i,
					      match)) == 1) {
				vector_free(cmd_vector);
				*status = CMD_ERR_AMBIGUOUS;
				return NULL;
			}
			/*
			   else if (ret == 2)
			   {
			   vector_free (cmd_vector);
			   *status = CMD_ERR_NO_MATCH;
			   return NULL;
			   }
			 */
		}

	/* Prepare match vector. */
	matchvec = vector_init(INIT_MATCHVEC_SIZE);

	/* Now we got into completion */
	for (i = 0; i < vector_active(cmd_vector); i++)
		if ((cmd_element = vector_slot(cmd_vector, i))) {
			const char *string;
			vector strvec = cmd_element->strvec;

			/* Check field length */
			if (index >= vector_active(strvec))
				vector_slot(cmd_vector, i) = NULL;
			else {
				unsigned int j;

				descvec = vector_slot(strvec, index);
				for (j = 0; j < vector_active(descvec); j++)
					if ((desc = vector_slot(descvec, j))) {
						if ((string = cmd_entry_function(vector_slot(vline, index), desc->cmd)))
							if (cmd_unique_string (matchvec, string))
								vector_set (matchvec, talloc_strdup(tall_vty_cmd_ctx, string));
					}
			}
		}

	/* We don't need cmd_vector any more. */
	vector_free(cmd_vector);

	/* No matched command */
	if (vector_slot(matchvec, 0) == NULL) {
		vector_free(matchvec);

		/* In case of 'command \t' pattern.  Do you need '?' command at
		   the end of the line. */
		if (vector_slot(vline, index) == '\0')
			*status = CMD_ERR_NOTHING_TODO;
		else
			*status = CMD_ERR_NO_MATCH;
		return NULL;
	}

	/* Only one matched */
	if (vector_slot(matchvec, 1) == NULL) {
		match_str = (char **)matchvec->index;
		vector_only_wrapper_free(matchvec);
		*status = CMD_COMPLETE_FULL_MATCH;
		return match_str;
	}
	/* Make it sure last element is NULL. */
	vector_set(matchvec, NULL);

	/* Check LCD of matched strings. */
	if (vector_slot(vline, index) != NULL) {
		lcd = cmd_lcd((char **)matchvec->index);

		if (lcd) {
			int len = strlen(vector_slot(vline, index));

			if (len < lcd) {
				char *lcdstr;

				lcdstr = _talloc_zero(tall_vty_cmd_ctx, lcd + 1,
						      "complete-lcdstr");
				memcpy(lcdstr, matchvec->index[0], lcd);
				lcdstr[lcd] = '\0';

				/* match_str = (char **) &lcdstr; */

				/* Free matchvec. */
				for (i = 0; i < vector_active(matchvec); i++) {
					if (vector_slot(matchvec, i))
						talloc_free(vector_slot(matchvec, i));
				}
				vector_free(matchvec);

				/* Make new matchvec. */
				matchvec = vector_init(INIT_MATCHVEC_SIZE);
				vector_set(matchvec, lcdstr);
				match_str = (char **)matchvec->index;
				vector_only_wrapper_free(matchvec);

				*status = CMD_COMPLETE_MATCH;
				return match_str;
			}
		}
	}

	match_str = (char **)matchvec->index;
	vector_only_wrapper_free(matchvec);
	*status = CMD_COMPLETE_LIST_MATCH;
	return match_str;
}

char **cmd_complete_command(vector vline, struct vty *vty, int *status)
{
	char **ret;

	if (cmd_try_do_shortcut(vty->node, vector_slot(vline, 0))) {
		enum node_type onode;
		vector shifted_vline;
		unsigned int index;

		onode = vty->node;
		vty->node = ENABLE_NODE;
		/* We can try it on enable node, cos' the vty is authenticated */

		shifted_vline = vector_init(vector_count(vline));
		/* use memcpy? */
		for (index = 1; index < vector_active(vline); index++) {
			vector_set_index(shifted_vline, index - 1,
					 vector_lookup(vline, index));
		}

		ret = cmd_complete_command_real(shifted_vline, vty, status);

		vector_free(shifted_vline);
		vty->node = onode;
		return ret;
	}

	return cmd_complete_command_real(vline, vty, status);
}

static struct vty_parent_node *vty_parent(struct vty *vty)
{
	return llist_first_entry_or_null(&vty->parent_nodes,
					 struct vty_parent_node,
					 entry);
}

static bool vty_pop_parent(struct vty *vty)
{
	struct vty_parent_node *parent = vty_parent(vty);
	if (!parent) {
		printf("vty_pop_parent() returns false, vty->node unchanged = %d\n", vty->node);
		return false;
	}
	printf("vty_pop_parent() on vty->node = %d\n", vty->node);
	llist_del(&parent->entry);
	vty->node = parent->node;
	vty->priv = parent->priv;
	if (vty->indent)
		talloc_free(vty->indent);
	vty->indent = parent->indent;
	printf("vty->indent: "); print_str(vty->indent);
	talloc_free(parent);
	return true;
}

static void vty_clear_parents(struct vty *vty)
{
	while (vty_pop_parent(vty));
}

/* return parent node */
/*
 * This function MUST eventually converge on a node when called repeatedly,
 * there must not be any cycles.
 * All 'config' nodes shall converge on CONFIG_NODE.
 * All other 'enable' nodes shall converge on ENABLE_NODE.
 * All 'view' only nodes shall converge on VIEW_NODE.
 * All other nodes shall converge on themselves or it must be ensured,
 * that the user's rights are not extended anyhow by calling this function.
 *
 * Note that these requirements also apply to all functions that are used
 * as go_parent_cb.
 * Note also that this function relies on the is_config_child callback to
 * recognize non-config nodes if go_parent_cb is not set.
 */
int vty_go_parent(struct vty *vty)
{
	printf("vty_go_parent() vty->node = %d\n", vty->node);
	switch (vty->node) {
		case AUTH_NODE:
		case VIEW_NODE:
		case ENABLE_NODE:
		case CONFIG_NODE:
			vty_clear_parents(vty);
			break;

		case AUTH_ENABLE_NODE:
			vty->node = VIEW_NODE;
			vty_clear_parents(vty);
			break;

		case CFG_LOG_NODE:
		case VTY_NODE:
			vty->node = CONFIG_NODE;
			vty_clear_parents(vty);
			break;

		default:
			if (host.app_info->go_parent_cb) {
				host.app_info->go_parent_cb(vty);
				printf("called go_parent_cb(), vty->node = %d\n", vty->node);
				vty_pop_parent(vty);
			}
			else if (is_config_child(vty)) {
				vty->node = CONFIG_NODE;
				printf("is_config_child() returned true, vty->node = CONFIG_NODE = %d\n", vty->node);
				vty_clear_parents(vty);
			}
			else {
				vty->node = VIEW_NODE;
				printf("is_config_child() returned false, vty->node = VIEW_NODE = %d\n", vty->node);
				vty_clear_parents(vty);
			}
			break;
#if 0
			if (host.app_info->go_parent_cb)
				host.app_info->go_parent_cb(vty);
			if (!vty_pop_parent(vty)) {
				printf("vty_pop_parent() returned false, vty->node = %d\n", vty->node);
				if (is_config_child(vty)) {
					vty->node = CONFIG_NODE;
					printf("is_config_child() returned true, vty->node = CONFIG_NODE = %d\n", vty->node);
					vty_clear_parents(vty);
				}
				else {
					vty->node = VIEW_NODE;
					printf("is_config_child() returned false, vty->node = VIEW_NODE = %d\n", vty->node);
					vty_clear_parents(vty);
				}
			}
			else
				printf("vty_pop_parent() returned true, vty->node = %d\n", vty->node);
			break;
#endif
	}

	return vty->node;
}

/* Execute command by argument vline vector. */
static int
cmd_execute_command_real(vector vline, struct vty *vty,
			 struct cmd_element **cmd)
{
	unsigned int i;
	unsigned int index;
	vector cmd_vector;
	struct cmd_element *cmd_element;
	struct cmd_element *matched_element;
	unsigned int matched_count, incomplete_count;
	int argc;
	const char *argv[CMD_ARGC_MAX];
	enum match_type match = 0;
	int varflag;
	char *command;

	/* Make copy of command elements. */
	cmd_vector = vector_copy(cmd_node_vector(cmdvec, vty->node));

	for (index = 0; index < vector_active(vline); index++)
		if ((command = vector_slot(vline, index))) {
			int ret;

			match = cmd_filter(command, cmd_vector, index,
			                   any_match);

			if (match == vararg_match)
				break;

			ret =
			    is_cmd_ambiguous(command, cmd_vector, index, match);

			if (ret == 1) {
				vector_free(cmd_vector);
				return CMD_ERR_AMBIGUOUS;
			} else if (ret == 2) {
				vector_free(cmd_vector);
				return CMD_ERR_NO_MATCH;
			}
		}

	/* Check matched count. */
	matched_element = NULL;
	matched_count = 0;
	incomplete_count = 0;

	for (i = 0; i < vector_active(cmd_vector); i++)
		if ((cmd_element = vector_slot(cmd_vector, i))) {
			if (match == vararg_match
			    || index >= cmd_element->cmdsize) {
				matched_element = cmd_element;
#if 0
				printf("DEBUG: %s\n", cmd_element->string);
#endif
				matched_count++;
			} else {
				incomplete_count++;
			}
		}

	/* Finish of using cmd_vector. */
	vector_free(cmd_vector);

	/* To execute command, matched_count must be 1. */
	if (matched_count == 0) {
		if (incomplete_count)
			return CMD_ERR_INCOMPLETE;
		else
			return CMD_ERR_NO_MATCH;
	}

	if (matched_count > 1)
		return CMD_ERR_AMBIGUOUS;

	/* Argument treatment */
	varflag = 0;
	argc = 0;

	for (i = 0; i < vector_active(vline); i++) {
		if (varflag)
			argv[argc++] = vector_slot(vline, i);
		else {
			vector descvec =
			    vector_slot(matched_element->strvec, i);

			if (vector_active(descvec) == 1) {
				struct desc *desc = vector_slot(descvec, 0);

				if (CMD_VARARG(desc->cmd))
					varflag = 1;

				if (varflag || CMD_VARIABLE(desc->cmd)
				    || CMD_OPTION(desc->cmd))
					argv[argc++] = vector_slot(vline, i);
			} else
				argv[argc++] = vector_slot(vline, i);
		}

		if (argc >= CMD_ARGC_MAX)
			return CMD_ERR_EXEED_ARGC_MAX;
	}

	/* For vtysh execution. */
	if (cmd)
		*cmd = matched_element;

	if (matched_element->daemon)
		return CMD_SUCCESS_DAEMON;

	/* Execute matched command. */
	return (*matched_element->func) (matched_element, vty, argc, argv);
}

int
cmd_execute_command(vector vline, struct vty *vty, struct cmd_element **cmd,
		    int vtysh)
{
	int ret;
	enum node_type onode;

	onode = vty->node;

	if (cmd_try_do_shortcut(vty->node, vector_slot(vline, 0))) {
		vector shifted_vline;
		unsigned int index;

		vty->node = ENABLE_NODE;
		/* We can try it on enable node, cos' the vty is authenticated */

		shifted_vline = vector_init(vector_count(vline));
		/* use memcpy? */
		for (index = 1; index < vector_active(vline); index++) {
			vector_set_index(shifted_vline, index - 1,
					 vector_lookup(vline, index));
		}

		ret = cmd_execute_command_real(shifted_vline, vty, cmd);

		vector_free(shifted_vline);
		vty->node = onode;
		return ret;
	}

	return cmd_execute_command_real(vline, vty, cmd);
}

/* Execute command by argument readline. */
int
cmd_execute_command_strict(vector vline, struct vty *vty,
			   struct cmd_element **cmd)
{
	unsigned int i;
	unsigned int index;
	vector cmd_vector;
	struct cmd_element *cmd_element;
	struct cmd_element *matched_element;
	unsigned int matched_count, incomplete_count;
	int argc;
	const char *argv[CMD_ARGC_MAX];
	int varflag;
	enum match_type match = 0;
	char *command;

	/* Make copy of command element */
	cmd_vector = vector_copy(cmd_node_vector(cmdvec, vty->node));

	for (index = 0; index < vector_active(vline); index++)
		if ((command = vector_slot(vline, index))) {
			int ret;

			match = cmd_filter(vector_slot(vline, index),
			                   cmd_vector, index, exact_match);

			/* If command meets '.VARARG' then finish matching. */
			if (match == vararg_match)
				break;

			ret =
			    is_cmd_ambiguous(command, cmd_vector, index, match);
			if (ret == 1) {
				vector_free(cmd_vector);
				return CMD_ERR_AMBIGUOUS;
			}
			if (ret == 2) {
				vector_free(cmd_vector);
				return CMD_ERR_NO_MATCH;
			}
		}

	/* Check matched count. */
	matched_element = NULL;
	matched_count = 0;
	incomplete_count = 0;
	for (i = 0; i < vector_active(cmd_vector); i++)
		if (vector_slot(cmd_vector, i) != NULL) {
			cmd_element = vector_slot(cmd_vector, i);

			if (match == vararg_match
			    || index >= cmd_element->cmdsize) {
				matched_element = cmd_element;
				matched_count++;
			} else
				incomplete_count++;
		}

	/* Finish of using cmd_vector. */
	vector_free(cmd_vector);

	/* To execute command, matched_count must be 1. */
	if (matched_count == 0) {
		if (incomplete_count)
			return CMD_ERR_INCOMPLETE;
		else
			return CMD_ERR_NO_MATCH;
	}

	if (matched_count > 1)
		return CMD_ERR_AMBIGUOUS;

	/* Argument treatment */
	varflag = 0;
	argc = 0;

	for (i = 0; i < vector_active(vline); i++) {
		if (varflag)
			argv[argc++] = vector_slot(vline, i);
		else {
			vector descvec =
			    vector_slot(matched_element->strvec, i);

			if (vector_active(descvec) == 1) {
				struct desc *desc = vector_slot(descvec, 0);

				if (CMD_VARARG(desc->cmd))
					varflag = 1;

				if (varflag || CMD_VARIABLE(desc->cmd)
				    || CMD_OPTION(desc->cmd))
					argv[argc++] = vector_slot(vline, i);
			} else
				argv[argc++] = vector_slot(vline, i);
		}

		if (argc >= CMD_ARGC_MAX)
			return CMD_ERR_EXEED_ARGC_MAX;
	}

	/* For vtysh execution. */
	if (cmd)
		*cmd = matched_element;

	if (matched_element->daemon)
		return CMD_SUCCESS_DAEMON;

	/* Now execute matched command */
	return (*matched_element->func) (matched_element, vty, argc, argv);
}

static inline size_t len(const char *str)
{
	return str? strlen(str) : 0;
}

/*! Make sure the common length of strings a and b is identical, then compare their lengths. I.e., if a
 * is longer than b, a must start with exactly b, and vice versa.
 * \returns EINVAL on mismatch, -1 for a < b, 0 for a == b, 1 for a > b.
 */
static int _indent_cmp(const char *a, const char *b)
{
	size_t al, bl;
	al = len(a);
	bl = len(b);
	if (al > bl) {
		if (bl && strncmp(a, b, bl) != 0)
			return EINVAL;
		return 1;
	}
	/* al <= bl */
	if (al && strncmp(a, b, al) != 0)
		return EINVAL;
	return (al < bl)? -1 : 0;
}
static int __indent_cmp(const char *a, const char *b, const char *a_literal, const char *b_literal)
{
	int cmp;
	printf("indent_cmp(%s, %s)\n", a_literal, b_literal);
	printf("a: "); print_str(a);
	printf("b: "); print_str(b);
	cmp = _indent_cmp(a, b);
	printf("cmp = %d\n", cmp);
	return cmp;
}
#define indent_cmp(a,b) __indent_cmp(a, b, #a, #b)
	
/* Configration make from file. */
int config_from_file(struct vty *vty, FILE * fp)
{
	int ret;
	vector vline;
	char *indent;
	int cmp;
	struct vty_parent_node this_node;
	struct vty_parent_node *parent;
	printf("start\n");
	printf("CONFIG_NODE = %d\n", CONFIG_NODE);

	while (fgets(vty->buf, VTY_BUFSIZ, fp)) {
		indent = NULL;
		vline = NULL;
		ret = cmd_make_strvec2(vty->buf, &indent, &vline);
		printf("\n\nvty->node = %d\n", vty->node);
		printf("line: "); print_str(vty->buf);
		printf("indent: "); print_str(indent);
		printf("vty->indent: "); print_str(vty->indent);

		if (ret != CMD_SUCCESS)
			goto return_invalid_indent;

		/* In case of comment or empty line */
		if (vline == NULL) {
			if (indent) {
				talloc_free(indent);
				indent = NULL;
			}
			continue;
		}

		/* We have a nonempty line. */
		if (!vty->indent) {
			/* We have just entered a node and expecting the first child to come up; but we
			 * may also skip right back to a parent or ancestor level. */
			parent = vty_parent(vty);

			printf("!vty->indent; parent->indent: "); print_str(parent? parent->indent : NULL);

			/* If there is no parent, record any indentation we encounter. */
			cmp = parent ? indent_cmp(indent, parent->indent) : 1;

			if (cmp == EINVAL)
				goto return_invalid_indent;

			if (cmp <= 0) {
				/* We have gone right back to the parent level or higher, we are skipping
				 * this child node level entirely. Pop the parent to go back to a node
				 * that was actually there (to reinstate vty->indent) and re-use below
				 * go-parent while-loop to find an accurate match of indent in the node
				 * ancestry. */
				printf("skipping child level\n");
				vty_go_parent(vty);
			} else {
				/* The indent is deeper than the just entered parent, record the new
				 * indentation characters. */
				vty->indent = talloc_strdup(vty, indent);
				/* This *is* the new indentation. */
				cmp = 0;
				printf("remember indent: vty->indent: "); print_str(vty->indent);
			}
		} else {
			/* There is a known indentation for this node level, validate and detect node
			 * exits. */
			cmp = indent_cmp(indent, vty->indent);
			if (cmp == EINVAL)
				goto return_invalid_indent;
		}

		/* Less indent: go up the parent nodes to find matching amount of less indent. When this
		 * loop exits, we want to have found an exact match, i.e. cmp == 0. */
		while (cmp < 0) {
			vty_go_parent(vty);
			cmp = indent_cmp(indent, vty->indent);
			if (cmp == EINVAL)
				goto return_invalid_indent;
		}

		/* More indent without having entered a child node level? Either the parent node's indent
		 * wasn't hit exactly (e.g. there's a space more than the parent level had further above)
		 * or the indentation increased even though the vty command didn't enter a child. */
		if (cmp > 0)
			goto return_invalid_indent;

		/* Remember the current node before the command possibly changes it. */
		this_node = (struct vty_parent_node){
				.node = vty->node,
				.priv = vty->priv,
				.indent = vty->indent,
			};

		parent = vty_parent(vty);
		ret = cmd_execute_command_strict(vline, vty, NULL);
		cmd_free_strvec(vline);

		if (ret != CMD_SUCCESS && ret != CMD_WARNING
		    && ret != CMD_ERR_NOTHING_TODO) {
			if (indent) {
				talloc_free(indent);
				indent = NULL;
			}
			return ret;
		}

		/* If we have stepped down into a child node, push a parent frame.
		 * The causality is such: we don't expect every single node entry implementation to push
		 * a parent node entry onto vty->parent_nodes. Instead we expect vty_go_parent() to *pop*
		 * a parent node. Hence if the node changed without the parent node changing, we must
		 * have stepped into a child node (and now expect a deeper indent). */
		if (vty->node != this_node.node && parent == vty_parent(vty)) {
			printf("push parent\n");
			/* Push the parent node. */
			parent = talloc_zero(vty, struct vty_parent_node);
			*parent = this_node;
			printf("parent->indent: "); print_str(parent->indent);
			llist_add(&parent->entry, &vty->parent_nodes);

			/* The current talloc'ed vty->indent string will now be owned by this parent
			 * struct. Indicate that we don't know what deeper indent characters the user
			 * will choose. */
			vty->indent = NULL;
		}

		if (indent) {
			talloc_free(indent);
			indent = NULL;
		}
	}
	return CMD_SUCCESS;

return_invalid_indent:
	printf("invalid indent");
	if (vline)
		cmd_free_strvec(vline);
	if (indent) {
		talloc_free(indent);
		indent = NULL;
	}
	return CMD_ERR_INVALID_INDENT;
}

/* Configration from terminal */
DEFUN(config_terminal,
      config_terminal_cmd,
      "configure terminal",
      "Configuration from vty interface\n" "Configuration terminal\n")
{
	if (vty_config_lock(vty))
		vty->node = CONFIG_NODE;
	else {
		vty_out(vty, "VTY configuration is locked by other VTY%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}
	return CMD_SUCCESS;
}

/* Enable command */
DEFUN(enable, config_enable_cmd, "enable", "Turn on privileged mode command\n")
{
	/* If enable password is NULL, change to ENABLE_NODE */
	if ((host.enable == NULL && host.enable_encrypt == NULL) ||
	    vty->type == VTY_SHELL_SERV)
		vty->node = ENABLE_NODE;
	else
		vty->node = AUTH_ENABLE_NODE;

	return CMD_SUCCESS;
}

/* Disable command */
DEFUN(disable,
      config_disable_cmd, "disable", "Turn off privileged mode command\n")
{
	if (vty->node == ENABLE_NODE)
		vty->node = VIEW_NODE;
	return CMD_SUCCESS;
}

/* Down vty node level. */
gDEFUN(config_exit,
      config_exit_cmd, "exit", "Exit current mode and down to previous mode\n")
{
	switch (vty->node) {
	case AUTH_NODE:
	case VIEW_NODE:
	case ENABLE_NODE:
		vty->status = VTY_CLOSE;
		break;
	case CONFIG_NODE:
		vty->node = ENABLE_NODE;
		vty_config_unlock(vty);
		break;
	default:
		if (vty->node > CONFIG_NODE)
			vty_go_parent (vty);
		break;
	}
	return CMD_SUCCESS;
}

/* End of configuration. */
    gDEFUN(config_end,
      config_end_cmd, "end", "End current mode and change to enable mode.")
{
	if (vty->node > ENABLE_NODE) {
		int last_node = CONFIG_NODE;

		/* Repeatedly call go_parent until a top node is reached. */
		while (vty->node > CONFIG_NODE) {
			if (vty->node == last_node) {
				/* Ensure termination, this shouldn't happen. */
				break;
			}
			last_node = vty->node;
			vty_go_parent(vty);
		}

		vty_config_unlock(vty);
		if (vty->node > ENABLE_NODE)
			vty->node = ENABLE_NODE;
		vty->index = NULL;
		vty->index_sub = NULL;
	}
	return CMD_SUCCESS;
}

/* Show version. */
DEFUN(show_version,
      show_version_cmd, "show version", SHOW_STR "Displays program version\n")
{
	vty_out(vty, "%s %s (%s).%s", host.app_info->name,
		host.app_info->version,
		host.app_info->name ? host.app_info->name : "", VTY_NEWLINE);
	vty_out(vty, "%s%s", host.app_info->copyright, VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(show_online_help,
      show_online_help_cmd, "show online-help", SHOW_STR "Online help\n")
{
	vty_dump_nodes(vty);
	return CMD_SUCCESS;
}

/* Help display function for all node. */
gDEFUN(config_help,
      config_help_cmd, "help", "Description of the interactive help system\n")
{
	vty_out(vty,
		"This VTY provides advanced help features.  When you need help,%s\
anytime at the command line please press '?'.%s\
%s\
If nothing matches, the help list will be empty and you must backup%s\
 until entering a '?' shows the available options.%s\
Two styles of help are provided:%s\
1. Full help is available when you are ready to enter a%s\
command argument (e.g. 'show ?') and describes each possible%s\
argument.%s\
2. Partial help is provided when an abbreviated argument is entered%s\
   and you want to know what arguments match the input%s\
   (e.g. 'show me?'.)%s%s", VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE,
		VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE, VTY_NEWLINE);
	return CMD_SUCCESS;
}

/* Help display function for all node. */
gDEFUN(config_list, config_list_cmd, "list", "Print command list\n")
{
	unsigned int i;
	struct cmd_node *cnode = vector_slot(cmdvec, vty->node);
	struct cmd_element *cmd;

	for (i = 0; i < vector_active(cnode->cmd_vector); i++)
		if ((cmd = vector_slot(cnode->cmd_vector, i)) != NULL
		    && !(cmd->attr == CMD_ATTR_DEPRECATED
			 || cmd->attr == CMD_ATTR_HIDDEN))
			vty_out(vty, "  %s%s", cmd->string, VTY_NEWLINE);
	return CMD_SUCCESS;
}

static int write_config_file(const char *config_file, char **outpath)
{
	unsigned int i;
	int fd;
	struct cmd_node *node;
	char *config_file_tmp = NULL;
	char *config_file_sav = NULL;
	struct vty *file_vty;
	struct stat st;

	*outpath = NULL;

	/* Check and see if we are operating under vtysh configuration */
	config_file_sav =
	    _talloc_zero(tall_vty_cmd_ctx,
			 strlen(config_file) + strlen(CONF_BACKUP_EXT) + 1,
			 "config_file_sav");
	strcpy(config_file_sav, config_file);
	strcat(config_file_sav, CONF_BACKUP_EXT);

	config_file_tmp = _talloc_zero(tall_vty_cmd_ctx, strlen(config_file) + 8,
					"config_file_tmp");
	sprintf(config_file_tmp, "%s.XXXXXX", config_file);

	/* Open file to configuration write. */
	fd = mkstemp(config_file_tmp);
	if (fd < 0) {
		*outpath = talloc_strdup(tall_vty_cmd_ctx, config_file_tmp);
		talloc_free(config_file_tmp);
		talloc_free(config_file_sav);
		return -1;
	}

	/* Make vty for configuration file. */
	file_vty = vty_new();
	file_vty->fd = fd;
	file_vty->type = VTY_FILE;

	/* Config file header print. */
	vty_out(file_vty, "!\n! %s (%s) configuration saved from vty\n!",
		host.app_info->name, host.app_info->version);
	//vty_time_print (file_vty, 1);
	vty_out(file_vty, "!\n");

	for (i = 0; i < vector_active(cmdvec); i++)
		if ((node = vector_slot(cmdvec, i)) && node->func) {
			if ((*node->func) (file_vty))
				vty_out(file_vty, "!\n");
		}
	vty_close(file_vty);

	if (unlink(config_file_sav) != 0)
		if (errno != ENOENT) {
			*outpath = talloc_strdup(tall_vty_cmd_ctx, config_file_sav);
			talloc_free(config_file_sav);
			talloc_free(config_file_tmp);
			unlink(config_file_tmp);
			return -2;
		}

	/* Only link the .sav file if the original file exists */
	if (stat(config_file, &st) == 0) {
		if (link(config_file, config_file_sav) != 0) {
			*outpath = talloc_strdup(tall_vty_cmd_ctx, config_file_sav);
			talloc_free(config_file_sav);
			talloc_free(config_file_tmp);
			unlink(config_file_tmp);
			return -3;
		}
		sync();
		if (unlink(config_file) != 0) {
			*outpath = talloc_strdup(tall_vty_cmd_ctx, config_file);
			talloc_free(config_file_sav);
			talloc_free(config_file_tmp);
			unlink(config_file_tmp);
			return -4;
		}
	}
	if (link(config_file_tmp, config_file) != 0) {
		*outpath = talloc_strdup(tall_vty_cmd_ctx, config_file);
		talloc_free(config_file_sav);
		talloc_free(config_file_tmp);
		unlink(config_file_tmp);
		return -5;
	}
	unlink(config_file_tmp);
	sync();

	talloc_free(config_file_sav);
	talloc_free(config_file_tmp);

	if (chmod(config_file, 0666 & ~CONFIGFILE_MASK) != 0) {
		*outpath = talloc_strdup(tall_vty_cmd_ctx, config_file);
		return -6;
	}

	return 0;
}


/* Write current configuration into file. */
DEFUN(config_write_file,
      config_write_file_cmd,
      "write file",
      "Write running configuration to memory, network, or terminal\n"
      "Write to configuration file\n")
{
	char *failed_file;
	int rc;

	if (host.app_info->config_is_consistent) {
		rc = host.app_info->config_is_consistent(vty);
		if (!rc) {
			vty_out(vty, "Configuration is not consistent%s",
				VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	if (host.config == NULL) {
		vty_out(vty, "Can't save to configuration file, using vtysh.%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	rc = write_config_file(host.config, &failed_file);
	switch (rc) {
	case -1:
		vty_out(vty, "Can't open configuration file %s.%s",
			failed_file, VTY_NEWLINE);
		rc = CMD_WARNING;
		break;
	case -2:
		vty_out(vty, "Can't unlink backup configuration file %s.%s",
			failed_file, VTY_NEWLINE);
		rc = CMD_WARNING;
		break;
	case -3:
		vty_out(vty, "Can't backup old configuration file %s.%s",
			failed_file, VTY_NEWLINE);
		rc = CMD_WARNING;
		break;
	case -4:
		vty_out(vty, "Can't unlink configuration file %s.%s",
			failed_file, VTY_NEWLINE);
		rc = CMD_WARNING;
		break;
	case -5:
		vty_out(vty, "Can't save configuration file %s.%s", failed_file,
			VTY_NEWLINE);
		rc = CMD_WARNING;
		break;
	case -6:
		vty_out(vty, "Can't chmod configuration file %s: %s (%d).%s",
			failed_file, strerror(errno), errno, VTY_NEWLINE);
		rc = CMD_WARNING;
		break;
	default:
		vty_out(vty, "Configuration saved to %s%s", host.config, VTY_NEWLINE);
		rc = CMD_SUCCESS;
		break;
	}

	talloc_free(failed_file);
	return rc;
}

ALIAS(config_write_file,
      config_write_cmd,
      "write", "Write running configuration to memory, network, or terminal\n")

    ALIAS(config_write_file,
      config_write_memory_cmd,
      "write memory",
      "Write running configuration to memory, network, or terminal\n"
      "Write configuration to the file (same as write file)\n")

    ALIAS(config_write_file,
      copy_runningconfig_startupconfig_cmd,
      "copy running-config startup-config",
      "Copy configuration\n"
      "Copy running config to... \n"
      "Copy running config to startup config (same as write file)\n")

/* Write current configuration into the terminal. */
    DEFUN(config_write_terminal,
      config_write_terminal_cmd,
      "write terminal",
      "Write running configuration to memory, network, or terminal\n"
      "Write to terminal\n")
{
	unsigned int i;
	struct cmd_node *node;

	if (vty->type == VTY_SHELL_SERV) {
		for (i = 0; i < vector_active(cmdvec); i++)
			if ((node = vector_slot(cmdvec, i)) && node->func
			    && node->vtysh) {
				if ((*node->func) (vty))
					vty_out(vty, "!%s", VTY_NEWLINE);
			}
	} else {
		vty_out(vty, "%sCurrent configuration:%s", VTY_NEWLINE,
			VTY_NEWLINE);
		vty_out(vty, "!%s", VTY_NEWLINE);

		for (i = 0; i < vector_active(cmdvec); i++)
			if ((node = vector_slot(cmdvec, i)) && node->func) {
				if ((*node->func) (vty))
					vty_out(vty, "!%s", VTY_NEWLINE);
			}
		vty_out(vty, "end%s", VTY_NEWLINE);
	}
	return CMD_SUCCESS;
}

/* Write current configuration into the terminal. */
ALIAS(config_write_terminal,
      show_running_config_cmd,
      "show running-config", SHOW_STR "running configuration\n")

/* Write startup configuration into the terminal. */
    DEFUN(show_startup_config,
      show_startup_config_cmd,
      "show startup-config", SHOW_STR "Contentes of startup configuration\n")
{
	char buf[BUFSIZ];
	FILE *confp;

	confp = fopen(host.config, "r");
	if (confp == NULL) {
		vty_out(vty, "Can't open configuration file [%s]%s",
			host.config, VTY_NEWLINE);
		return CMD_WARNING;
	}

	while (fgets(buf, BUFSIZ, confp)) {
		char *cp = buf;

		while (*cp != '\r' && *cp != '\n' && *cp != '\0')
			cp++;
		*cp = '\0';

		vty_out(vty, "%s%s", buf, VTY_NEWLINE);
	}

	fclose(confp);

	return CMD_SUCCESS;
}

/* Hostname configuration */
DEFUN(config_hostname,
      hostname_cmd,
      "hostname WORD",
      "Set system's network name\n" "This system's network name\n")
{
	if (!isalpha((int)*argv[0])) {
		vty_out(vty, "Please specify string starting with alphabet%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (host.name)
		talloc_free(host.name);

	host.name = talloc_strdup(tall_vty_cmd_ctx, argv[0]);
	return CMD_SUCCESS;
}

DEFUN(config_no_hostname,
      no_hostname_cmd,
      "no hostname [HOSTNAME]",
      NO_STR "Reset system's network name\n" "Host name of this router\n")
{
	if (host.name)
		talloc_free(host.name);
	host.name = NULL;
	return CMD_SUCCESS;
}

/* VTY interface password set. */
DEFUN(config_password, password_cmd,
      "password (8|) WORD",
      "Assign the terminal connection password\n"
      "Specifies a HIDDEN password will follow\n"
      "dummy string \n" "The HIDDEN line password string\n")
{
	/* Argument check. */
	if (argc == 0) {
		vty_out(vty, "Please specify password.%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (argc == 2) {
		if (*argv[0] == '8') {
			if (host.password)
				talloc_free(host.password);
			host.password = NULL;
			if (host.password_encrypt)
				talloc_free(host.password_encrypt);
			host.password_encrypt = talloc_strdup(tall_vty_cmd_ctx, argv[1]);
			return CMD_SUCCESS;
		} else {
			vty_out(vty, "Unknown encryption type.%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	if (!isalnum((int)*argv[0])) {
		vty_out(vty,
			"Please specify string starting with alphanumeric%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (host.password)
		talloc_free(host.password);
	host.password = NULL;

#ifdef VTY_CRYPT_PW
	if (host.encrypt) {
		if (host.password_encrypt)
			talloc_free(host.password_encrypt);
		host.password_encrypt = talloc_strdup(tall_vty_cmd_ctx, zencrypt(argv[0]));
	} else
#endif
		host.password = talloc_strdup(tall_vty_cmd_ctx, argv[0]);

	return CMD_SUCCESS;
}

ALIAS(config_password, password_text_cmd,
      "password LINE",
      "Assign the terminal connection password\n"
      "The UNENCRYPTED (cleartext) line password\n")

/* VTY enable password set. */
    DEFUN(config_enable_password, enable_password_cmd,
      "enable password (8|) WORD",
      "Modify enable password parameters\n"
      "Assign the privileged level password\n"
      "Specifies a HIDDEN password will follow\n"
      "dummy string \n" "The HIDDEN 'enable' password string\n")
{
	/* Argument check. */
	if (argc == 0) {
		vty_out(vty, "Please specify password.%s", VTY_NEWLINE);
		return CMD_WARNING;
	}

	/* Crypt type is specified. */
	if (argc == 2) {
		if (*argv[0] == '8') {
			if (host.enable)
				talloc_free(host.enable);
			host.enable = NULL;

			if (host.enable_encrypt)
				talloc_free(host.enable_encrypt);
			host.enable_encrypt = talloc_strdup(tall_vty_cmd_ctx, argv[1]);

			return CMD_SUCCESS;
		} else {
			vty_out(vty, "Unknown encryption type.%s", VTY_NEWLINE);
			return CMD_WARNING;
		}
	}

	if (!isalnum((int)*argv[0])) {
		vty_out(vty,
			"Please specify string starting with alphanumeric%s",
			VTY_NEWLINE);
		return CMD_WARNING;
	}

	if (host.enable)
		talloc_free(host.enable);
	host.enable = NULL;

	/* Plain password input. */
#ifdef VTY_CRYPT_PW
	if (host.encrypt) {
		if (host.enable_encrypt)
			talloc_free(host.enable_encrypt);
		host.enable_encrypt = talloc_strdup(tall_vty_cmd_ctx, zencrypt(argv[0]));
	} else
#endif
		host.enable = talloc_strdup(tall_vty_cmd_ctx, argv[0]);

	return CMD_SUCCESS;
}

ALIAS(config_enable_password,
      enable_password_text_cmd,
      "enable password LINE",
      "Modify enable password parameters\n"
      "Assign the privileged level password\n"
      "The UNENCRYPTED (cleartext) 'enable' password\n")

/* VTY enable password delete. */
    DEFUN(no_config_enable_password, no_enable_password_cmd,
      "no enable password",
      NO_STR
      "Modify enable password parameters\n"
      "Assign the privileged level password\n")
{
	if (host.enable)
		talloc_free(host.enable);
	host.enable = NULL;

	if (host.enable_encrypt)
		talloc_free(host.enable_encrypt);
	host.enable_encrypt = NULL;

	return CMD_SUCCESS;
}

#ifdef VTY_CRYPT_PW
DEFUN(service_password_encrypt,
      service_password_encrypt_cmd,
      "service password-encryption",
      "Set up miscellaneous service\n" "Enable encrypted passwords\n")
{
	if (host.encrypt)
		return CMD_SUCCESS;

	host.encrypt = 1;

	if (host.password) {
		if (host.password_encrypt)
			talloc_free(host.password_encrypt);
		host.password_encrypt = talloc_strdup(tall_vty_cmd_ctx, zencrypt(host.password));
	}
	if (host.enable) {
		if (host.enable_encrypt)
			talloc_free(host.enable_encrypt);
		host.enable_encrypt = talloc_strdup(tall_vty_cmd_ctx, zencrypt(host.enable));
	}

	return CMD_SUCCESS;
}

DEFUN(no_service_password_encrypt,
      no_service_password_encrypt_cmd,
      "no service password-encryption",
      NO_STR "Set up miscellaneous service\n" "Enable encrypted passwords\n")
{
	if (!host.encrypt)
		return CMD_SUCCESS;

	host.encrypt = 0;

	if (host.password_encrypt)
		talloc_free(host.password_encrypt);
	host.password_encrypt = NULL;

	if (host.enable_encrypt)
		talloc_free(host.enable_encrypt);
	host.enable_encrypt = NULL;

	return CMD_SUCCESS;
}
#endif

DEFUN(config_terminal_length, config_terminal_length_cmd,
      "terminal length <0-512>",
      "Set terminal line parameters\n"
      "Set number of lines on a screen\n"
      "Number of lines on screen (0 for no pausing)\n")
{
	int lines;
	char *endptr = NULL;

	lines = strtol(argv[0], &endptr, 10);
	if (lines < 0 || lines > 512 || *endptr != '\0') {
		vty_out(vty, "length is malformed%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	vty->lines = lines;

	return CMD_SUCCESS;
}

DEFUN(config_terminal_no_length, config_terminal_no_length_cmd,
      "terminal no length",
      "Set terminal line parameters\n"
      NO_STR "Set number of lines on a screen\n")
{
	vty->lines = -1;
	return CMD_SUCCESS;
}

DEFUN(service_terminal_length, service_terminal_length_cmd,
      "service terminal-length <0-512>",
      "Set up miscellaneous service\n"
      "System wide terminal length configuration\n"
      "Number of lines of VTY (0 means no line control)\n")
{
	int lines;
	char *endptr = NULL;

	lines = strtol(argv[0], &endptr, 10);
	if (lines < 0 || lines > 512 || *endptr != '\0') {
		vty_out(vty, "length is malformed%s", VTY_NEWLINE);
		return CMD_WARNING;
	}
	host.lines = lines;

	return CMD_SUCCESS;
}

DEFUN(no_service_terminal_length, no_service_terminal_length_cmd,
      "no service terminal-length [<0-512>]",
      NO_STR
      "Set up miscellaneous service\n"
      "System wide terminal length configuration\n"
      "Number of lines of VTY (0 means no line control)\n")
{
	host.lines = -1;
	return CMD_SUCCESS;
}

DEFUN_HIDDEN(do_echo,
	     echo_cmd,
	     "echo .MESSAGE",
	     "Echo a message back to the vty\n" "The message to echo\n")
{
	char *message;

	vty_out(vty, "%s%s",
		((message =
		  argv_concat(argv, argc, 0)) ? message : ""), VTY_NEWLINE);
	if (message)
		talloc_free(message);
	return CMD_SUCCESS;
}

#if 0
DEFUN(config_logmsg,
      config_logmsg_cmd,
      "logmsg " LOG_LEVELS " .MESSAGE",
      "Send a message to enabled logging destinations\n"
      LOG_LEVEL_DESC "The message to send\n")
{
	int level;
	char *message;

	if ((level = level_match(argv[0])) == ZLOG_DISABLED)
		return CMD_ERR_NO_MATCH;

	zlog(NULL, level,
	     ((message = argv_concat(argv, argc, 1)) ? message : ""));
	if (message)
		talloc_free(message);
	return CMD_SUCCESS;
}

DEFUN(show_logging,
      show_logging_cmd,
      "show logging", SHOW_STR "Show current logging configuration\n")
{
	struct zlog *zl = zlog_default;

	vty_out(vty, "Syslog logging: ");
	if (zl->maxlvl[ZLOG_DEST_SYSLOG] == ZLOG_DISABLED)
		vty_out(vty, "disabled");
	else
		vty_out(vty, "level %s, facility %s, ident %s",
			zlog_priority[zl->maxlvl[ZLOG_DEST_SYSLOG]],
			facility_name(zl->facility), zl->ident);
	vty_out(vty, "%s", VTY_NEWLINE);

	vty_out(vty, "Stdout logging: ");
	if (zl->maxlvl[ZLOG_DEST_STDOUT] == ZLOG_DISABLED)
		vty_out(vty, "disabled");
	else
		vty_out(vty, "level %s",
			zlog_priority[zl->maxlvl[ZLOG_DEST_STDOUT]]);
	vty_out(vty, "%s", VTY_NEWLINE);

	vty_out(vty, "Monitor logging: ");
	if (zl->maxlvl[ZLOG_DEST_MONITOR] == ZLOG_DISABLED)
		vty_out(vty, "disabled");
	else
		vty_out(vty, "level %s",
			zlog_priority[zl->maxlvl[ZLOG_DEST_MONITOR]]);
	vty_out(vty, "%s", VTY_NEWLINE);

	vty_out(vty, "File logging: ");
	if ((zl->maxlvl[ZLOG_DEST_FILE] == ZLOG_DISABLED) || !zl->fp)
		vty_out(vty, "disabled");
	else
		vty_out(vty, "level %s, filename %s",
			zlog_priority[zl->maxlvl[ZLOG_DEST_FILE]],
			zl->filename);
	vty_out(vty, "%s", VTY_NEWLINE);

	vty_out(vty, "Protocol name: %s%s",
		zlog_proto_names[zl->protocol], VTY_NEWLINE);
	vty_out(vty, "Record priority: %s%s",
		(zl->record_priority ? "enabled" : "disabled"), VTY_NEWLINE);

	return CMD_SUCCESS;
}

DEFUN(config_log_stdout,
      config_log_stdout_cmd,
      "log stdout", "Logging control\n" "Set stdout logging level\n")
{
	zlog_set_level(NULL, ZLOG_DEST_STDOUT, zlog_default->default_lvl);
	return CMD_SUCCESS;
}

DEFUN(config_log_stdout_level,
      config_log_stdout_level_cmd,
      "log stdout " LOG_LEVELS,
      "Logging control\n" "Set stdout logging level\n" LOG_LEVEL_DESC)
{
	int level;

	if ((level = level_match(argv[0])) == ZLOG_DISABLED)
		return CMD_ERR_NO_MATCH;
	zlog_set_level(NULL, ZLOG_DEST_STDOUT, level);
	return CMD_SUCCESS;
}

DEFUN(no_config_log_stdout,
      no_config_log_stdout_cmd,
      "no log stdout [LEVEL]",
      NO_STR "Logging control\n" "Cancel logging to stdout\n" "Logging level\n")
{
	zlog_set_level(NULL, ZLOG_DEST_STDOUT, ZLOG_DISABLED);
	return CMD_SUCCESS;
}

DEFUN(config_log_monitor,
      config_log_monitor_cmd,
      "log monitor",
      "Logging control\n" "Set terminal line (monitor) logging level\n")
{
	zlog_set_level(NULL, ZLOG_DEST_MONITOR, zlog_default->default_lvl);
	return CMD_SUCCESS;
}

DEFUN(config_log_monitor_level,
      config_log_monitor_level_cmd,
      "log monitor " LOG_LEVELS,
      "Logging control\n"
      "Set terminal line (monitor) logging level\n" LOG_LEVEL_DESC)
{
	int level;

	if ((level = level_match(argv[0])) == ZLOG_DISABLED)
		return CMD_ERR_NO_MATCH;
	zlog_set_level(NULL, ZLOG_DEST_MONITOR, level);
	return CMD_SUCCESS;
}

DEFUN(no_config_log_monitor,
      no_config_log_monitor_cmd,
      "no log monitor [LEVEL]",
      NO_STR
      "Logging control\n"
      "Disable terminal line (monitor) logging\n" "Logging level\n")
{
	zlog_set_level(NULL, ZLOG_DEST_MONITOR, ZLOG_DISABLED);
	return CMD_SUCCESS;
}

static int set_log_file(struct vty *vty, const char *fname, int loglevel)
{
	int ret;
	char *p = NULL;
	const char *fullpath;

	/* Path detection. */
	if (!IS_DIRECTORY_SEP(*fname)) {
		char cwd[MAXPATHLEN + 1];
		cwd[MAXPATHLEN] = '\0';

		if (getcwd(cwd, MAXPATHLEN) == NULL) {
			zlog_err("config_log_file: Unable to alloc mem!");
			return CMD_WARNING;
		}

		if ((p = _talloc_zero(tall_vcmd_ctx,
				      strlen(cwd) + strlen(fname) + 2),
				      "set_log_file")
		    == NULL) {
			zlog_err("config_log_file: Unable to alloc mem!");
			return CMD_WARNING;
		}
		sprintf(p, "%s/%s", cwd, fname);
		fullpath = p;
	} else
		fullpath = fname;

	ret = zlog_set_file(NULL, fullpath, loglevel);

	if (p)
		talloc_free(p);

	if (!ret) {
		vty_out(vty, "can't open logfile %s\n", fname);
		return CMD_WARNING;
	}

	if (host.logfile)
		talloc_free(host.logfile);

	host.logfile = talloc_strdup(tall_vty_cmd_ctx, fname);

	return CMD_SUCCESS;
}

DEFUN(config_log_file,
      config_log_file_cmd,
      "log file FILENAME",
      "Logging control\n" "Logging to file\n" "Logging filename\n")
{
	return set_log_file(vty, argv[0], zlog_default->default_lvl);
}

DEFUN(config_log_file_level,
      config_log_file_level_cmd,
      "log file FILENAME " LOG_LEVELS,
      "Logging control\n"
      "Logging to file\n" "Logging filename\n" LOG_LEVEL_DESC)
{
	int level;

	if ((level = level_match(argv[1])) == ZLOG_DISABLED)
		return CMD_ERR_NO_MATCH;
	return set_log_file(vty, argv[0], level);
}

DEFUN(no_config_log_file,
      no_config_log_file_cmd,
      "no log file [FILENAME]",
      NO_STR
      "Logging control\n" "Cancel logging to file\n" "Logging file name\n")
{
	zlog_reset_file(NULL);

	if (host.logfile)
		talloc_free(host.logfile);

	host.logfile = NULL;

	return CMD_SUCCESS;
}

ALIAS(no_config_log_file,
      no_config_log_file_level_cmd,
      "no log file FILENAME LEVEL",
      NO_STR
      "Logging control\n"
      "Cancel logging to file\n" "Logging file name\n" "Logging level\n")

    DEFUN(config_log_syslog,
      config_log_syslog_cmd,
      "log syslog", "Logging control\n" "Set syslog logging level\n")
{
	zlog_set_level(NULL, ZLOG_DEST_SYSLOG, zlog_default->default_lvl);
	return CMD_SUCCESS;
}

DEFUN(config_log_syslog_level,
      config_log_syslog_level_cmd,
      "log syslog " LOG_LEVELS,
      "Logging control\n" "Set syslog logging level\n" LOG_LEVEL_DESC)
{
	int level;

	if ((level = level_match(argv[0])) == ZLOG_DISABLED)
		return CMD_ERR_NO_MATCH;
	zlog_set_level(NULL, ZLOG_DEST_SYSLOG, level);
	return CMD_SUCCESS;
}

DEFUN_DEPRECATED(config_log_syslog_facility,
		 config_log_syslog_facility_cmd,
		 "log syslog facility " LOG_FACILITIES,
		 "Logging control\n"
		 "Logging goes to syslog\n"
		 "(Deprecated) Facility parameter for syslog messages\n"
		 LOG_FACILITY_DESC)
{
	int facility;

	if ((facility = facility_match(argv[0])) < 0)
		return CMD_ERR_NO_MATCH;

	zlog_set_level(NULL, ZLOG_DEST_SYSLOG, zlog_default->default_lvl);
	zlog_default->facility = facility;
	return CMD_SUCCESS;
}

DEFUN(no_config_log_syslog,
      no_config_log_syslog_cmd,
      "no log syslog [LEVEL]",
      NO_STR "Logging control\n" "Cancel logging to syslog\n" "Logging level\n")
{
	zlog_set_level(NULL, ZLOG_DEST_SYSLOG, ZLOG_DISABLED);
	return CMD_SUCCESS;
}

ALIAS(no_config_log_syslog,
      no_config_log_syslog_facility_cmd,
      "no log syslog facility " LOG_FACILITIES,
      NO_STR
      "Logging control\n"
      "Logging goes to syslog\n"
      "Facility parameter for syslog messages\n" LOG_FACILITY_DESC)

    DEFUN(config_log_facility,
      config_log_facility_cmd,
      "log facility " LOG_FACILITIES,
      "Logging control\n"
      "Facility parameter for syslog messages\n" LOG_FACILITY_DESC)
{
	int facility;

	if ((facility = facility_match(argv[0])) < 0)
		return CMD_ERR_NO_MATCH;
	zlog_default->facility = facility;
	return CMD_SUCCESS;
}

DEFUN(no_config_log_facility,
      no_config_log_facility_cmd,
      "no log facility [FACILITY]",
      NO_STR
      "Logging control\n"
      "Reset syslog facility to default (daemon)\n" "Syslog facility\n")
{
	zlog_default->facility = LOG_DAEMON;
	return CMD_SUCCESS;
}

DEFUN_DEPRECATED(config_log_trap,
		 config_log_trap_cmd,
		 "log trap " LOG_LEVELS,
		 "Logging control\n"
		 "(Deprecated) Set logging level and default for all destinations\n"
		 LOG_LEVEL_DESC)
{
	int new_level;
	int i;

	if ((new_level = level_match(argv[0])) == ZLOG_DISABLED)
		return CMD_ERR_NO_MATCH;

	zlog_default->default_lvl = new_level;
	for (i = 0; i < ZLOG_NUM_DESTS; i++)
		if (zlog_default->maxlvl[i] != ZLOG_DISABLED)
			zlog_default->maxlvl[i] = new_level;
	return CMD_SUCCESS;
}

DEFUN_DEPRECATED(no_config_log_trap,
		 no_config_log_trap_cmd,
		 "no log trap [LEVEL]",
		 NO_STR
		 "Logging control\n"
		 "Permit all logging information\n" "Logging level\n")
{
	zlog_default->default_lvl = LOG_DEBUG;
	return CMD_SUCCESS;
}

DEFUN(config_log_record_priority,
      config_log_record_priority_cmd,
      "log record-priority",
      "Logging control\n"
      "Log the priority of the message within the message\n")
{
	zlog_default->record_priority = 1;
	return CMD_SUCCESS;
}

DEFUN(no_config_log_record_priority,
      no_config_log_record_priority_cmd,
      "no log record-priority",
      NO_STR
      "Logging control\n"
      "Do not log the priority of the message within the message\n")
{
	zlog_default->record_priority = 0;
	return CMD_SUCCESS;
}
#endif

DEFUN(banner_motd_file,
      banner_motd_file_cmd,
      "banner motd file [FILE]",
      "Set banner\n" "Banner for motd\n" "Banner from a file\n" "Filename\n")
{
	if (host.motdfile)
		talloc_free(host.motdfile);
	host.motdfile = talloc_strdup(tall_vty_cmd_ctx, argv[0]);

	return CMD_SUCCESS;
}

DEFUN(banner_motd_default,
      banner_motd_default_cmd,
      "banner motd default",
      "Set banner string\n" "Strings for motd\n" "Default string\n")
{
	host.motd = default_motd;
	return CMD_SUCCESS;
}

DEFUN(no_banner_motd,
      no_banner_motd_cmd,
      "no banner motd", NO_STR "Set banner string\n" "Strings for motd\n")
{
	host.motd = NULL;
	if (host.motdfile)
		talloc_free(host.motdfile);
	host.motdfile = NULL;
	return CMD_SUCCESS;
}

/* Set config filename.  Called from vty.c */
void host_config_set(const char *filename)
{
	host.config = talloc_strdup(tall_vty_cmd_ctx, filename);
}

void install_default(int node)
{
	install_element(node, &config_help_cmd);
	install_element(node, &config_list_cmd);

	install_element(node, &config_write_terminal_cmd);
	install_element(node, &config_write_file_cmd);
	install_element(node, &config_write_memory_cmd);
	install_element(node, &config_write_cmd);
	install_element(node, &show_running_config_cmd);
}

void vty_install_default(int node)
{
	install_default(node);

	install_element(node, &config_exit_cmd);

	if (node >= CONFIG_NODE) {
		/* It's not a top node. */
		install_element(node, &config_end_cmd);
	}
}

/**
 * Write the current running config to a given file
 * \param[in] vty the vty of the code
 * \param[in] filename where to store the file
 * \return 0 in case of success.
 *
 * If the filename already exists create a filename.sav
 * version with the current code.
 *
 */
int osmo_vty_write_config_file(const char *filename)
{
	char *failed_file;
	int rc;

	rc = write_config_file(filename, &failed_file);
	talloc_free(failed_file);
	return rc;
}

/**
 * Save the current state to the config file
 * \return 0 in case of success.
 *
 * If the filename already exists create a filename.sav
 * version with the current code.
 *
 */
int osmo_vty_save_config_file(void)
{
	char *failed_file;
	int rc;

	if (host.config == NULL)
		return -7;

	rc = write_config_file(host.config, &failed_file);
	talloc_free(failed_file);
	return rc;
}

/* Initialize command interface. Install basic nodes and commands. */
void cmd_init(int terminal)
{
	/* Allocate initial top vector of commands. */
	cmdvec = vector_init(VECTOR_MIN_SIZE);

	/* Default host value settings. */
	host.name = NULL;
	host.password = NULL;
	host.enable = NULL;
	host.logfile = NULL;
	host.config = NULL;
	host.lines = -1;
	host.motd = default_motd;
	host.motdfile = NULL;

	/* Install top nodes. */
	install_node(&view_node, NULL);
	install_node(&enable_node, NULL);
	install_node(&auth_node, NULL);
	install_node(&auth_enable_node, NULL);
	install_node(&config_node, config_write_host);

	/* Each node's basic commands. */
	install_element(VIEW_NODE, &show_version_cmd);
	install_element(VIEW_NODE, &show_online_help_cmd);
	if (terminal) {
		install_element(VIEW_NODE, &config_list_cmd);
		install_element(VIEW_NODE, &config_exit_cmd);
		install_element(VIEW_NODE, &config_help_cmd);
		install_element(VIEW_NODE, &config_enable_cmd);
		install_element(VIEW_NODE, &config_terminal_length_cmd);
		install_element(VIEW_NODE, &config_terminal_no_length_cmd);
		install_element(VIEW_NODE, &echo_cmd);
	}

	if (terminal) {
		vty_install_default(ENABLE_NODE);
		install_element(ENABLE_NODE, &config_disable_cmd);
		install_element(ENABLE_NODE, &config_terminal_cmd);
		install_element (ENABLE_NODE, &copy_runningconfig_startupconfig_cmd);
	}
	install_element (ENABLE_NODE, &show_startup_config_cmd);
	install_element(ENABLE_NODE, &show_version_cmd);
	install_element(ENABLE_NODE, &show_online_help_cmd);

	if (terminal) {
		install_element(ENABLE_NODE, &config_terminal_length_cmd);
		install_element(ENABLE_NODE, &config_terminal_no_length_cmd);
		install_element(ENABLE_NODE, &echo_cmd);

		vty_install_default(CONFIG_NODE);
	}

	install_element(CONFIG_NODE, &hostname_cmd);
	install_element(CONFIG_NODE, &no_hostname_cmd);

	if (terminal) {
		install_element(CONFIG_NODE, &password_cmd);
		install_element(CONFIG_NODE, &password_text_cmd);
		install_element(CONFIG_NODE, &enable_password_cmd);
		install_element(CONFIG_NODE, &enable_password_text_cmd);
		install_element(CONFIG_NODE, &no_enable_password_cmd);

#ifdef VTY_CRYPT_PW
		install_element(CONFIG_NODE, &service_password_encrypt_cmd);
		install_element(CONFIG_NODE, &no_service_password_encrypt_cmd);
#endif
		install_element(CONFIG_NODE, &banner_motd_default_cmd);
		install_element(CONFIG_NODE, &banner_motd_file_cmd);
		install_element(CONFIG_NODE, &no_banner_motd_cmd);
		install_element(CONFIG_NODE, &service_terminal_length_cmd);
		install_element(CONFIG_NODE, &no_service_terminal_length_cmd);

	}
	srand(time(NULL));
}

/*! @} */
