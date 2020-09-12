#line 106 "mdcode.mdc"
#define _GNU_SOURCE
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

#include "mdcode.h"
#line 493 "mdcode.mdc"
#include  <ctype.h>
#include  <string.h>

#line 194 "mdcode.mdc"
struct psection {
	struct section;
	struct code_node *last;
	int refcnt;
	int indent;
};

#line 231 "mdcode.mdc"
static void code_linearize(struct code_node *code)
{
	struct code_node *t;
	for (t = code; t; t = t->next)
		t->indent = 0;
	for (; code; code = code->next)
		if (code->child) {
			struct code_node *next = code->next;
			struct psection *pchild =
				(struct	psection *)code->child;
			int indent = pchild->indent;
			code->next = code->child->code;
			code->child->code = NULL;
			code->child = NULL;
			for (t = code; t->next; t = t->next)
				t->next->indent = code->indent + indent;
			t->next = next;
		}
}

#line 254 "mdcode.mdc"
void code_free(struct code_node *code)
{
	while (code) {
		struct code_node *this;
		if (code->child)
			code_linearize(code);
		this = code;
		code = code->next;
		free(this);
	}
}

#line 283 "mdcode.mdc"
static void code_add_text(struct psection *where, struct text txt,
			  int line_no, int needs_strip)
{
	struct code_node *n;
	if (txt.len == 0)
		return;
	n = malloc(sizeof(*n));
	n->code = txt;
	n->indent = 0;
	n->line_no = line_no;
	if (needs_strip) {
		if (txt.txt[0] == '\t')
			n->needs_strip = 8;
		else
			n->needs_strip = 4;
	} else
		n->needs_strip = 0;
	n->next = NULL;
	n->child = NULL;
	if (where->last)
		where->last->next = n;
	else
		where->code = n;
	where->last = n;
}

#line 312 "mdcode.mdc"
void code_add_link(struct psection *where, struct psection *to,
		   int indent)
{
	struct code_node *n;

	to->indent = indent;
	to->refcnt++;	// this will be checked elsewhere
	if (where->last && where->last->child == NULL) {
		where->last->child = to;
		return;
	}
	n = malloc(sizeof(*n));
	n->code.len = 0;
	n->indent = 0;
	n->line_no = 0;
	n->next = NULL;
	n->child = to;
	if (where->last)
		where->last->next = n;
	else
		where->code = n;
	where->last = n;
}

#line 356 "mdcode.mdc"
int text_cmp(struct text a, struct text b)
{
	int len = a.len;
	if (len > b.len)
		len = b.len;
	int cmp = strncmp(a.txt, b.txt, len);
	if (cmp)
		return cmp;
	else
		return a.len - b.len;
}

static struct psection *section_find(struct psection **list, struct text name)
{
	struct psection *new;
	while (*list) {
		int cmp = text_cmp((*list)->section, name);
		if (cmp == 0)
			return *list;
		if (cmp > 0)
			break;
		list = (struct psection **)&((*list)->next);
	}
	/* Add this section */
	new = malloc(sizeof(*new));
	new->next = *list;
	*list = new;
	new->section = name;
	new->code = NULL;
	new->last = NULL;
	new->refcnt = 0;
	new->indent = 0;
	return new;
}

#line 442 "mdcode.mdc"
static char *skip_lws(char *pos, char *end)
{
	while (pos < end && (*pos == ' ' || *pos == '\t'))
		pos++;
	return pos;
}

static char *skip_line(char *pos, char *end)
{
	while (pos < end && *pos != '\n')
		pos++;
	if (pos < end)
		pos++;
	return pos;
}

static char *skip_para(char *pos, char *end, int *line_no)
{
	/* Might return a pointer to a blank line, as only
	 * one trailing blank line is skipped
	 */
	if (*pos == '#') {
		pos = skip_line(pos, end);
		(*line_no) += 1;
		return pos;
	}
	while (pos < end &&
	       *pos != '#' &&
	       *(pos = skip_lws(pos, end)) != '\n') {
		pos = skip_line(pos, end);
		(*line_no) += 1;
	}
	if (pos < end && *pos == '\n') {
		pos++;
		(*line_no) += 1;
	}
	return pos;
}

#line 498 "mdcode.mdc"
static struct text take_header(char *pos, char *end)
{
	struct text section;

	while (pos < end && *pos == '#')
		pos++;
	while (pos < end && *pos == ' ')
		pos++;
	section.txt = pos;
	while (pos < end && *pos != '\n')
		pos++;
	while (pos > section.txt &&
	       (pos[-1] == '#' || pos[-1] == ' '))
		pos--;
	section.len = pos - section.txt;
	return section;
}

static int is_list(char *pos, char *end)
{
	if (strchr("-*+", *pos))
		return 1;
	if (isdigit(*pos)) {
		while (pos < end && isdigit(*pos))
			pos += 1;
		if  (pos < end && *pos == '.')
			return 1;
	}
	return 0;
}

static int matches(char *start, char *pos, char *end)
{
	if (start == NULL)
		return matches("\t", pos, end) ||
		       matches("    ", pos, end);
	return (pos + strlen(start) < end &&
		strncmp(pos, start, strlen(start)) == 0);
}

#line 575 "mdcode.mdc"
static int count_space(char *sol, char *p)
{
	int c = 0;
	while (sol < p) {
		if (sol[0] == ' ')
			c++;
		if (sol[0] == '\t')
			c+= 8;
		sol++;
	}
	return c;
}

static char *take_code(char *pos, char *end, char *marker,
		       struct psection **table, struct text section,
		       int *line_nop)
{
	char *start = pos;
	int line_no = *line_nop;
	int start_line = line_no;
	struct psection *sect;

	sect = section_find(table, section);

	while (pos < end) {
		char *sol, *t;
		struct text ref;

		if (marker && matches(marker, pos, end))
			break;
		if (!marker &&
		    (skip_lws(pos, end))[0] != '\n' &&
		    !matches(NULL, pos, end))
			/* Paragraph not indented */
			break;

		/* Still in code - check for reference */
		sol = pos;
		if (!marker) {
			if (*sol == '\t')
				sol++;
			else if (strcmp(sol, "    ") == 0)
				sol += 4;
		}
		t = skip_lws(sol, end);
		if (t[0] != '#' || t[1] != '#') {
			/* Just regular code here */
			pos = skip_line(sol, end);
			line_no++;
			continue;
		}

		if (pos > start) {
			struct text txt;
			txt.txt = start;
			txt.len = pos - start;
			code_add_text(sect, txt, start_line,
			              marker == NULL);
		}
		ref = take_header(t, end);
		if (ref.len) {
			struct psection *refsec = section_find(table, ref);
			code_add_link(sect, refsec, count_space(sol, t));
		}
		pos = skip_line(t, end);
		line_no++;
		start = pos;
		start_line = line_no;
	}
	if (pos > start) {
		struct text txt;
		txt.txt = start;
		txt.len = pos - start;
		/* strip trailing blank lines */
		while (!marker && txt.len > 2 &&
		       start[txt.len-1] == '\n' &&
		       start[txt.len-2] == '\n')
			txt.len -= 1;

		code_add_text(sect, txt, start_line,
		              marker == NULL);
	}
	if (marker) {
		pos = skip_line(pos, end);
		line_no++;
	}
	*line_nop = line_no;
	return pos;
}

#line 674 "mdcode.mdc"
static struct psection *code_find(char *pos, char *end)
{
	struct psection *table = NULL;
	int in_list = 0;
	int line_no = 1;
	struct text section = {0};

	while (pos < end) {
		if (pos[0] == '#') {
			section = take_header(pos, end);
			in_list = 0;
			pos = skip_line(pos, end);
			line_no++;
		} else if (is_list(pos, end)) {
			in_list = 1;
			pos = skip_para(pos, end, &line_no);
		} else if (!in_list && matches(NULL, pos, end)) {
			pos = take_code(pos, end, NULL, &table,
					section, &line_no);
		} else if (matches("```", pos, end)) {
			in_list = 0;
			pos = skip_line(pos, end);
			line_no++;
			pos = take_code(pos, end, "```", &table,
					section, &line_no);
		} else if (matches("~~~", pos, end)) {
			in_list = 0;
			pos = skip_line(pos, end);
			line_no++;
			pos = take_code(pos, end, "~~~", &table,
					section, &line_no);
		} else {
			if (!isspace(*pos))
				in_list = 0;
			pos = skip_para(pos, end, &line_no);
		}
	}
	return table;
}

#line 734 "mdcode.mdc"
struct section *code_extract(char *pos, char *end, code_err_fn error)
{
	struct psection *table;
	struct section *result = NULL;
	struct section *tofree = NULL;

	table = code_find(pos, end);

	while (table) {
		struct psection *t = (struct psection*)table->next;
		if (table->last == NULL) {
			char *msg;
			asprintf(&msg,
				"Section \"%.*s\" is referenced but not declared",
				 table->section.len, table->section.txt);
			error(msg);
			free(msg);
		}
		if (table->refcnt == 0) {
			/* Root-section,  return it */
			table->next = result;
			result = table;
			code_linearize(result->code);
		} else {
			table->next = tofree;
			tofree = table;
			if (table->refcnt > 1) {
				char *msg;
				asprintf(&msg,
					 "Section \"%.*s\" referenced multiple times (%d).",
					 table->section.len, table->section.txt,
					 table->refcnt);
				error(msg);
				free(msg);
			}
		}
		table = t;
	}
	while (tofree) {
		struct section *t = tofree->next;
		free(tofree);
		tofree = t;
	}
	return result;
}

#line 814 "mdcode.mdc"
void code_node_print(FILE *out, struct code_node *node,
                     char *fname)
{
	for (; node; node = node->next) {
		char *c = node->code.txt;
		int len = node->code.len;

		if (!len)
			continue;

		fprintf(out, "#line %d \"%s\"\n",
			node->line_no, fname);
		while (len && *c) {
			if (node->indent >= 8)
				fprintf(out, "\t%*s", node->indent - 8, "");
			else
				fprintf(out, "%*s", node->indent, "");
			if (node->needs_strip) {
				if (*c == '\t' && len > 1) {
					c++;
					len--;
				} else if (strncmp(c, "    ", 4) == 0 && len > 4) {
					c += 4;
					len-= 4;
				}
			}
			do {
				fputc(*c, out);
				c++;
				len--;
			} while (len && c[-1] != '\n');
		}
	}
}

#line 115 "mdcode.mdc"

