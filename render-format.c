/*
 * Copyright Neil Brown ©2015-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * render-format.  Provide 'render-line' functions to render
 * a document one element per line using a format string to display
 * attributes of that element.
 *
 * This is particularly used for directories and the document list.
 */

#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>
#include "core.h"
#include "misc.h"

struct rf_data {
	char *format;
	unsigned short nfields;
	unsigned short alloc_fields;
	struct rf_field {
		/* 'field' can end at most one attribute, start at most one,
		 * can contain one text source, wither var or literal.
		 */
		char *val safe;	/* pointer into 'format' */
		char *attr;	/* pointer into 'format', or NULL */
		unsigned short attr_end;/* field where this attr ends */
		unsigned short attr_start;/* starting field for attr which ends here */
		unsigned short val_len;	/* length in 'format' */
		unsigned short attr_depth;
		short width;	/* min display width */
		unsigned short min_attr_depth; /* attr depth of first attr - from 0 */
		bool var;	/* else constant */
		char align;	/* l,r,c */
	} *fields safe;
};

static inline short FIELD_NUM(int i) { return i >> 16; }
static inline short FIELD_OFFSET(int i) { return i & 0xFFFF; }
static inline int MAKE_INDEX(short f, short i) { return (int)f << 16 | i;}

static char *do_format(struct pane *focus safe,
		       struct mark *m safe, struct mark *pm,
		       int len, int attrs)
{
	char *body = pane_attr_get(focus, "line-format");
	struct buf ret;
	char *n;

	if (pm && !mark_same(pm, m))
		pm = NULL;
	buf_init(&ret);

	if (!body)
		body = "%name";
	n = body;
	if (pm)
		goto endwhile;
	if (len >= 0 && ret.len >= len)
		goto endwhile;

	while (*n) {
		char buf[40], *b, *val;
		int w, adjust, l;

		if (!attrs && *n == '<' && n[1] != '<') {
			/* an attribute, skip it */
			n += 1;
			while (*n && *n != '>')
				n += 1;
			if (*n == '>')
				n += 1;
			continue;
		}
		if (*n != '%' || n[1] == '%') {
			buf_append_byte(&ret, *n);
			if (*n == '%')
				n += 1;
			n += 1;
			continue;
		}

		if (len >= 0 && ret.len >= len)
			break;
		if (pm)
			break;
		n += 1;
		b = buf;
		while (*n == '-' || *n == '_' || isalnum(*n)) {
			if (b < buf + sizeof(buf) - 2)
				*b++ = *n;
			n += 1;
		}
		*b = 0;
		if (!buf[0])
			val = "";
		else
			val = pane_mark_attr(focus, m, buf);
		if (!val)
			val = "-";

		if (*n != ':') {
			while (*val) {
				if (*val == '<' && attrs)
					buf_append_byte(&ret, '<');
				buf_append_byte(&ret, *val);
				val += 1;
			}
			continue;
		}
		w = 0;
		adjust=0;
		n += 1;
		while (*n) {
			if (isdigit(*n))
				w = w * 10 + (*n - '0');
			else if (w == 0 && *n == '-')
				adjust = 1;
			else break;
			n+= 1;
		}
		l = strlen(val);
		while (adjust && w > l) {
			buf_append(&ret, ' ');
			w -= 1;
		}

		while (*val && w > 0 ) {
			if (*val == '<' && attrs)
				buf_append_byte(&ret, '<');
			buf_append_byte(&ret, *val);
			w -= 1;
			val += 1;
		}
		while (w > 0) {
			buf_append(&ret, ' ');
			w -= 1;
		}
	}
endwhile:
	if (!*n) {
		if (len < 0)
			buf_append(&ret, '\n');
	}
	return buf_final(&ret);
}

DEF_CMD(format_content)
{
	if (!ci->mark || !ci->comm2)
		return Enoarg;
	if (ci->num)
		/* Cannot handle bytes */
		return Einval;

	while (doc_following(ci->focus, ci->mark) != WEOF) {
		const char *l, *c;
		wint_t w;

		l = do_format(ci->focus, ci->mark, NULL, -1, 0);
		if (!l)
			break;
		c = l;
		while (*c) {
			w = get_utf8(&c, NULL);
			if (w >= WERR ||
			    comm_call(ci->comm2, "consume", ci->home, w, ci->mark) <= 0)
				/* Finished */
				break;
		}
		free((void*)l);
		if (*c)
			break;
		doc_next(ci->focus, ci->mark);
	}
	return 1;
}

DEF_CMD(render_line)
{
	struct mark *m = ci->mark;
	struct mark *pm = ci->mark2;
	char *ret;
	int rv;
	int len;

	if (!ci->mark)
		return Enoarg;
	if (doc_following(ci->focus, ci->mark) == WEOF)
		return Efalse;

	if (pm && !mark_same(pm, m))
		pm = NULL;
	if (ci->num == NO_NUMERIC || ci->num < 0)
		len = -1;
	else
		len = ci->num;
	ret = do_format(ci->focus, ci->mark, pm, len, 1);
	if (!pm && len < 0)
		doc_next(ci->focus, m);
	rv = comm_call(ci->comm2, "callback:render", ci->focus, 0, NULL, ret);
	free(ret);
	return rv ?: 1;
}

DEF_CMD(render_line_prev)
{
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;
	if (RPT_NUM(ci) == 0)
		/* always at start-of-line */
		return 1;
	if (doc_prev(ci->focus, m) == WEOF)
		/* Hit start-of-file */
		return Efail;
	return 1;
}


DEF_CMD(format_free)
{
	struct rf_data *rf = ci->home->data;

	free(rf->fields);
	free(rf->format);
	unalloc(rf, pane);
	return 1;
}

static struct rf_field *new_field(struct rf_data *rd safe)
{
	struct rf_field *rf;

	if (rd->nfields >= rd->alloc_fields) {
		if (rd->alloc_fields >= 32768)
			return NULL;
		if (rd->alloc_fields < 8)
			rd->alloc_fields = 8;
		else
			rd->alloc_fields *= 2;
		rd->fields = realloc(rd->fields,
				     sizeof(*rd->fields) * rd->alloc_fields);
	}
	rf = &rd->fields[rd->nfields++];
	memset(rf, 0, sizeof(*rf));
	rf->attr_start = rd->nfields; /* i.e. no attr ends here */
	if (rd->nfields > 1)
		rf->attr_depth = rd->fields[rd->nfields-2].attr_depth;
	return rf;
}

static char *rf_add_field(struct rf_data *rd safe, char *str safe)
{
	struct rf_field *rf = new_field(rd);

	if (!rf)
		return NULL;

	if (str[0] == '<' && str[1] == '/' && str[2] == '>') {
		int start;
		str += 3;
		for (start = rd->nfields-2 ; start >= 0; start -= 1)
			if (rd->fields[start].attr &&
			    rd->fields[start].attr_end == 0)
				break;
		if (start >= 0) {
			rd->fields[start].attr_end = rd->nfields-1;
			rf->attr_start = start;
			rf->attr_depth -= 1;
		}
	}
	if (str[0] == '<' && str[1] != '<' && (str[1] != '/' || str[2] != '>')) {
		rf->attr = str+1;
		rf->attr_depth += 1;
		while (str[0] && str[0] != '>')
			str++;
		if (str[0])
			*str++ = 0;
	}
	if (str[0] == '<' && str[1] != '<')
		/* More attr start/stop, must go in next field */
		return str;

	if (str[0] != '%' || str[1] == '%') {
		/* Must be literal */
		rf->val = str;
		if (str[0] == '<' || str[0] == '%') {
			/* must be '<<' or '%%', only include second
			 * in ->val */
			rf->val += 1;
			str += 2;
			rf->val_len = 1;
		}
		while (str[0] && str[0] != '<' && str[0] != '%') {
			rf->val_len += 1;
			str += 1;
		}
		return str;
	}
	/* This is a '%' field */
	str += 1;
	rf->val = str;
	rf->align = 'l';
	rf->var = True;
	while (*str == '-' || *str == '_' || isalnum(*str))
		str += 1;
	rf->val_len = str - rf->val;
	if (*str != ':')
		return str;
	str += 1;
	if (*str == '-') {
		str += 1;
		rf->align = 'r';
	}
	while (isdigit(*str)) {
		rf->width *= 10;
		rf->width += *str - '0';
		str += 1;
	}
	return str;
}

static void set_format(struct pane *focus safe, struct rf_data *rd safe)
{
	char *str;
	int f;

	if (rd->format)
		return;
	str = pane_attr_get(focus, "line-format");
	rd->format = strdup(str ?: "%name");
	str = rd->format;
	while (str && *str)
		str = rf_add_field(rd, str);

	for (f = 0; f < rd->nfields; f++) {
		struct rf_field *rf = &rd->fields[f];

		if (rf->attr && rf->attr_end == 0)
			rf_add_field(rd, "</>");
	}
}

DEF_CMD(format_content2)
{
	/* doc:content delivers one char at a time to a callback.
	 * The chars are the apparent content, rather than the actual
	 * content.  So for a directory listing, it is the listing, not
	 * one newline per file.
	 * This is used for 'search' and 'copy'.
	 * This default version calls doc:step and is used when the actual
	 * and apparent content are the same.
	 *
	 * .mark is 'location': to start.  This is moved forwards
	 * .comm2 is 'consume': pass char mark and report if finished.
	 *
	 */
	struct mark *m = ci->mark;
	struct commcache dstep = CCINIT;
	int nxt;

	if (!m || !ci->comm2)
		return Enoarg;
	if (ci->num)
		/* Cannot handle bytes */
		return Einval;

	nxt = ccall(&dstep, "doc:step", ci->home, 1, m);
	while (nxt != CHAR_RET(WEOF) &&
	       comm_call(ci->comm2, "consume", ci->home, nxt, m) > 0) {
		ccall(&dstep, "doc:step", ci->home, 1, m, NULL, 1);
		nxt = ccall(&dstep, "doc:step", ci->home, 1, m);
	}

	return 1;
}

static int field_size(struct pane *p safe, struct mark *m safe, int field,
		      char **valp safe)
{
	struct rf_data *rd = p->data;
	struct rf_field *rf;
	char *val;
	int l;

	if (field < 0 || field > rd->nfields)
		return 0;
	if (field == rd->nfields) {
		/* Just a newline at the end */
		*valp = "\n";
		return 1;
	}
	rf = &rd->fields[field];
	if (!rf->var)
		return rf->val_len;
	val = *valp;
	if (!val) {
		char b[80];
		strncpy(b, rf->val, 80);
		b[79] = 0;
		if (rf->val_len < 80)
			b[rf->val_len] = 0;
		val = pane_mark_attr(p, m, b);
		if (!val)
			val = "-";
		*valp = val;
	}
	l = strlen(val);
	if (l < rf->width)
		return rf->width;
	else
		return l;
}

static int normalize(struct pane *home safe, struct mark *m safe, int inc)
{
	struct rf_data *rd = home->data;
	int index = m->ref.i;
	unsigned short f = FIELD_NUM(index);
	unsigned short o = FIELD_OFFSET(index);

	while (1) {
		char *val = NULL;
		int len;

		len = field_size(home, m, f, &val);
		if (o > len) {
			if (inc < 0)
				o = len;
			else
				return -1;
		}

		if (inc < 0) {
			if (o > 0) {
				o -= 1;
				break;
			}
			if (f == 0)
				return -1;
			/* Try previous field */
			f -= 1;
			o = 65535;
			continue;
		}
		if (inc > 0) {
			if (o < len) {
				o += 1;
				inc = 0;
			}
			if (o < len)
				break;
			if (f >= rd->nfields)
				return -1;
			/* Try next field */
			f += 1;
			o = 0;
			continue;
		}
		/* inc == 0 */
		if (len == 0) {
			if (f >= rd->nfields)
				return -1;
			/* Try next field */
			f += 1;
			o = 0;
			continue;
		}
		break;
	}
	return MAKE_INDEX(f, o);
}

static void prev_line(struct pane *home safe, struct mark *m safe)
{
	struct rf_data *rd = home->data;

	/* Move m to end of previous line, just before the newline */
	if (doc_prev(home->parent, m) == WEOF) {
		/* At the start already */
		m->ref.i = 0;
		return;
	}
	m->ref.i = MAKE_INDEX(rd->nfields, 0);
	mark_step(m, 0);
}

static void next_line(struct pane *home safe, struct mark *m safe)
{
	doc_next(home->parent, m);
	m->ref.i = MAKE_INDEX(0, 0);
	m->ref.i = normalize(home, m, 0);
	mark_step(m, 1);
}

DEF_CMD(format_step)
{
	struct rf_data *rd = ci->home->data;
	struct rf_field *rf;
	struct mark *m = ci->mark;
	int forward = ci->num;
	int move = ci->num2;
	int f, o;
	int margin;
	int fsize;
	int len = 0;
	char *val = NULL;
	int index;

	if (!m)
		return Enoarg;

	set_format(ci->focus, rd);

	if (!forward) {
		index = normalize(ci->home, m, -1);
		if (index < 0) {
			if (doc_prior(ci->home->parent, m) == WEOF)
				return CHAR_RET(WEOF);
			if (move)
				prev_line(ci->home, m);
			return CHAR_RET('\n');
		}
	} else {
		if (m->ref.p == NULL)
			return CHAR_RET(WEOF);
		index = normalize(ci->home, m, 0);
		if (index < 0)
			/* Should be impossible */
			return CHAR_RET(WEOF);
	}
	f = FIELD_NUM(index);
	o = FIELD_OFFSET(index);

	if (f >= rd->nfields) {
		if (move)
			next_line(ci->home, m);
		return CHAR_RET('\n');
	}
	rf = &rd->fields[f];
	fsize = field_size(ci->home, m, f, &val);
	if (val)
		len = strlen(val);
	if (move && forward) {
		mark_step(m, forward);
		index = normalize(ci->home, m, 1);
		if (index < 0) {
			next_line(ci->home, m);
			return CHAR_RET('\n');
		}
		m->ref.i = index;
	} else if (move && !forward) {
		mark_step(m, forward);
		m->ref.i = index;
	}

	if (!rf->var) {
		/* FIXME UTF-8 ?? */
		return CHAR_RET(rf->val[o]);
	}
	if (!val)
		return ' ';

	switch (rf->align) {
	case 'l':
	default:
		if (o < len)
			return CHAR_RET(val[o]);
		else
			return ' ';
	case 'c':
		margin = (fsize - len) / 2;
		if (margin < 0)
			margin = 0;
		if (o < margin)
			return ' ';
		if (o < margin + len)
			return CHAR_RET(val[o-margin]);
		return ' ';
	case 'r':
		margin = fsize - len;
		if (margin < 0)
			margin = 0;
		if (o < margin)
			return ' ';
		return CHAR_RET(val[o-margin]);
	}
}

DEF_CMD(format_attr)
{
	/* If there are attrs here, we report that by returning
	 * "render:format" as "yes"
	 */
	struct rf_data *rd = ci->home->data;
	struct mark *m = ci->mark;
	int previ;
	int f0, f;
	bool need_attr = False;

	if (!m || !ci->str)
		return Enoarg;
	if (!m->ref.p)
		return Efallthrough;
	if (ci->num2 == 0 && strcmp(ci->str, "render:format") != 0)
		return Efallthrough;
	if (ci->num2 && strncmp(ci->str, "render:format", strlen(ci->str)) != 0)
		return Efallthrough;

	if (FIELD_OFFSET(m->ref.i) > 0)
		/* attribute changes only happen at start of a field */
		return 1;

	/* There may be several previous fields that are empty.
	 * We need consider the possibility that any of those
	 * change the attributes.
	 */
	previ = normalize(ci->home, m, -1);
	if (previ < 0)
		f0 = 0;
	else if (FIELD_OFFSET(previ) > 0)
		f0 = FIELD_NUM(previ)+1;
	else
		f0 = FIELD_NUM(previ);
	for(f = f0; f <= FIELD_NUM(m->ref.i); f++) {
		if (f < rd->nfields) {
			if (rd->fields[f].attr_end > FIELD_NUM(m->ref.i) ||
			    rd->fields[f].attr_start < f0)
				need_attr = True;
		}
	}
	if (need_attr) {
		if (strcmp(ci->str, "render:format") == 0)
			comm_call(ci->comm2, "", ci->focus, 0, m, "yes");
		else
			comm_call(ci->comm2, "", ci->focus, 0, m, "yes",
				  0, NULL, "render:format");
	}
	return 1;
}

DEF_CMD(format_map)
{
	struct rf_data *rd = ci->home->data;
	struct mark *m = ci->mark;
	int previ;
	int f0, f;

	if (!ci->mark || !ci->str)
		return Enoarg;
	if (strcmp(ci->str, "render:format") != 0)
		return Efallthrough;
	if (ci->mark->ref.p == NULL)
		return Efallthrough;
	if (FIELD_OFFSET(m->ref.i) > 0)
		/* attribute changes only happen at start of a field */
		return 1;
	f = FIELD_NUM(ci->mark->ref.i);
	/* There may be several previous fields that are empty.
	 * We need consider the possibility that any of those
	 * change the attributes.
	 */
	previ = normalize(ci->home, m, -1);
	if (previ < 0)
		f0 = 0;
	else if (FIELD_OFFSET(previ) > 0)
		f0 = FIELD_NUM(previ)+1;
	else
		f0 = FIELD_NUM(previ);
	for(f = f0; f <= FIELD_NUM(m->ref.i); f++) {
		if (f >= rd->nfields)
			continue;
		/* Each depth gets a priority level from 20 up.
		 * When starting, set length to v.large.  When ending, set
		 * length to -1.
		 */
		if (rd->fields[f].attr_start < f0) {
			struct rf_field *st =
				&rd->fields[rd->fields[f].attr_start];
			comm_call(ci->comm2, "", ci->focus, -1, ci->mark,
				  st->attr, 20 + st->attr_depth);
		}
		if (rd->fields[f].attr_end > FIELD_NUM(m->ref.i)) {
			struct rf_field *st = &rd->fields[f];
			comm_call(ci->comm2, "", ci->focus, 32768, ci->mark,
				  st->attr, 20 + st->attr_depth);
		}
	}
	return 0;
}

DEF_CMD(render_line_prev2)
{
	struct mark *m = ci->mark;
	struct mark *m2, *mn;

	if (!m)
		return Enoarg;
	if (RPT_NUM(ci) == 0)
		;
	else if (doc_prev(ci->home->parent, m) == WEOF)
		/* Hit start-of-file */
		return Efail;
	m2 = m;
	while ((mn = mark_prev(m2)) != NULL &&
	       mn->ref.p == m2->ref.p &&
	       mn->ref.i > 0)
		m2 = mn;
	mark_to_mark(m, m2);
	m->ref.i = 0;

	return 1;
}

static struct pane *do_render_format_attach(struct pane *parent safe,
					    int nolines);
DEF_CMD(format_clone)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus, 0);
	pane_clone_children(ci->home, p);
	return 1;
}

static struct pane *do_render_format2_attach(struct pane *parent safe,
					     int nolines);
DEF_CMD(format_clone2)
{
	struct pane *p;

	p = do_render_format2_attach(ci->focus, 1);
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(format_noshare_ref)
{
	return Efalse;
}

static struct map *rf_map, *rf2_map;

static void render_format_register_map(void)
{
	rf_map = key_alloc();

	key_add(rf_map, "doc:render-line", &render_line);
	key_add(rf_map, "doc:render-line-prev", &render_line_prev);
	key_add(rf_map, "Clone", &format_clone);
	key_add(rf_map, "doc:content", &format_content);

	rf2_map = key_alloc();

	key_add(rf2_map, "doc:step", &format_step);
	key_add(rf2_map, "doc:get-attr", &format_attr);
	key_add(rf2_map, "map-attr", &format_map);
	key_add(rf2_map, "doc:render-line-prev", &render_line_prev2);
	key_add(rf2_map, "Clone", &format_clone2);
	key_add(rf2_map, "doc:content", &format_content2);
	key_add(rf2_map, "Free", &format_free);
	key_add(rf2_map, "doc:shares-ref", &format_noshare_ref);
}

DEF_LOOKUP_CMD(render_format_handle, rf_map);
DEF_LOOKUP_CMD(render_format2_handle, rf2_map);

static struct pane *do_render_format_attach(struct pane *parent safe,
					    int nolines)
{
	struct pane *p;

	if (!rf_map)
		render_format_register_map();

	p = pane_register(parent, 0, &render_format_handle.c);
	if (!p)
		return NULL;
	attr_set_str(&p->attrs, "render-wrap", "no");
	if (nolines)
		return p;
	return call_ret(pane, "attach-render-lines", p);
}

DEF_CMD(render_format_attach)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus, ci->num);
	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);
}

static struct pane *do_render_format2_attach(struct pane *parent safe,
					     int nolines)
{
	struct pane *p;
	struct rf_data *rf;

	if (!rf2_map)
		render_format_register_map();

	if (call("doc:shares-ref", parent) != 1)
		return NULL;

	alloc(rf, pane);
	p = pane_register(parent, 0, &render_format2_handle.c, rf);
	if (!p)
		return NULL;
	attr_set_str(&p->attrs, "render-wrap", "no");
	if (nolines)
		return p;
	return call_ret(pane, "attach-render-text", p);
}

DEF_CMD(render_format2_attach)
{
	struct pane *p;

	p = do_render_format2_attach(ci->focus, ci->num);
	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_format_attach, 0, NULL, "attach-render-format");
	call_comm("global-set-command", ed, &render_format2_attach, 0, NULL, "attach-render-format2");
}
