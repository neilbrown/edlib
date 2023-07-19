/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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

#define PANE_DATA_TYPE struct rf_data
#define DOC_NEXT(p,m,r,b) format_next_prev(p, ci->focus, m, r, 1, b)
#define DOC_PREV(p,m,r,b) format_next_prev(p, ci->focus, m, r, 0, b)
#define DOC_NEXT_DECL(p,m,r,b) format_next_prev(p, struct pane *focus safe, m, r, int forward, b)
#define DOC_PREV_DECL(p,m,r,b) format_next_prev(p, struct pane *focus safe, m, r, int forward, b)
#include "core.h"
#include "misc.h"

struct rf_data {
	char *format;
	unsigned short nfields;
	unsigned short alloc_fields;
	struct rf_field {
		/* 'field' can end at most one attribute, start at most one,
		 * can contain one text source, either var or literal.
		 */
		const char *val safe;	/* pointer into 'format' */
		const char *attr;	/* pointer into 'format', or NULL */
		unsigned short attr_end;/* field where this attr ends */
		unsigned short attr_start;/* starting field for attr which ends here */
		unsigned short val_len;	/* length in 'format' */
		unsigned short attr_depth;
		short width;	/* min display width */
		unsigned short min_attr_depth; /* attr depth of first attr - from 0 */
		bool var;	/* else constant */
		char align;	/* l,r,c */
	} *fields safe;
	char *attr_cache;
	void *cache_pos;
	int cache_field;
};

#include "core-pane.h"

static inline short FIELD_NUM(int i) { return i >> 16; }
static inline short FIELD_OFFSET(int i) { return i & 0xFFFF; }
static inline unsigned int MAKE_INDEX(short f, short i) { return (int)f << 16 | i;}

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
			n += 1;
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
	struct mark *m;

	if (!ci->mark || !ci->comm2)
		return Enoarg;
	if (ci->num)
		/* Cannot handle bytes */
		return Einval;

	m = mark_dup(ci->mark);
	while (doc_following(ci->focus, m) != WEOF) {
		const char *l, *c;
		wint_t w;

		l = do_format(ci->focus, m, NULL, -1, 0);
		if (!l)
			break;
		doc_next(ci->focus, m);
		c = l;
		while (*c) {
			w = get_utf8(&c, NULL);
			if (w >= WERR ||
			    comm_call(ci->comm2, "consume", ci->focus, w, m) <= 0)
				/* Finished */
				break;
		}
		free((void*)l);
		if (*c)
			break;
	}
	mark_free(m);
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
	if (ci->num < 0)
		len = -1;
	else
		len = ci->num;
	ret = do_format(ci->focus, ci->mark, pm, len, 1);
	if (len < 0)
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
	struct rf_data *rf = &ci->home->data;

	free(rf->attr_cache);
	free(rf->fields);
	free(rf->format);
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
			get_utf8((const char **)&str, NULL);
			rf->val_len += 1;
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

	for (f = rd->nfields - 1; f >= 0; f--) {
		struct rf_field *rf = &rd->fields[f];

		if (rf->attr && rf->attr_end == 0)
			rf_add_field(rd, "</>");
	}
}

static int field_size(struct pane *home safe, struct pane *focus safe,
		      struct mark *m safe, int field,
		      const char **valp safe)
{
	struct rf_data *rd = &home->data;
	struct rf_field *rf;
	const char *val;
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
	if (val)
		;
	else if (rd->attr_cache && rd->cache_field == field &&
		 rd->cache_pos == m->ref.p) {
		val = rd->attr_cache;
		*valp = strsave(home, val);
	} else {
		char b[80];
		strncpy(b, rf->val, 80);
		b[79] = 0;
		if (rf->val_len < 80)
			b[rf->val_len] = 0;
		val = pane_mark_attr(focus, m, b);
		if (!val)
			val = "-";
		*valp = val;
		if (rd->attr_cache)
			free(rd->attr_cache);
		rd->attr_cache = strdup(val);
		rd->cache_field = field;
		rd->cache_pos = m->ref.p;
	}
	l = utf8_strlen(val);
	if (l < rf->width)
		return rf->width;
	else
		return l;
}

static int normalize(struct pane *home safe, struct pane *focus safe,
		     struct mark *m safe, int inc)
{
	struct rf_data *rd = &home->data;
	int index = m->ref.i;
	unsigned short f = FIELD_NUM(index);
	unsigned short o = FIELD_OFFSET(index);

	while (1) {
		const char *val = NULL;
		int len;

		len = field_size(home, focus, m, f, &val);
		if (o > len) {
			if (inc == 0)
				return -1;
			if (inc < 0)
				o = len;
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
		if (o >= len) {
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

static void update_offset(struct mark *m safe, struct rf_data *rd safe,
			  unsigned int o)
{
	struct mark *m2 = m;
	struct mark *target = m;
	int f;

	/* If o is the first visible field, it needs to be 0 */
	if (o) {
		for (f = 0; f < rd->nfields; f++)
			if (rd->fields[f].var ||
			    rd->fields[f].val_len)
				break;
		if (o <= MAKE_INDEX(f, 0))
			o = 0;
	}
	if (m->ref.i == o)
		return;

	if (o > m->ref.i) {
		while (m2 && m2->ref.p == m->ref.p && m2->ref.i <= o) {
			target = m2;
			m2 = mark_next(m2);
		}
	} else {
		while (m2 && m2->ref.p == m->ref.p && m2->ref.i >= o) {
			target = m2;
			m2 = mark_prev(m2);
		}
	}
	m->ref.i = o;
	mark_to_mark_noref(m, target);
}

static void prev_line(struct pane *home safe, struct mark *m safe)
{
	struct rf_data *rd = &home->data;

	/* Move m to end of previous line, just before the newline */
	if (doc_prev(home->parent, m) == WEOF) {
		/* At the start already */
		update_offset(m, rd, 0);
		return;
	}
	update_offset(m, rd, MAKE_INDEX(rd->nfields, 0));
	mark_step(m, 0);
}

static void next_line(struct pane *home safe, struct mark *m safe)
{
	struct rf_data *rd = &home->data;

	doc_next(home->parent, m);
	update_offset(m, rd, MAKE_INDEX(0, 0));
	mark_step(m, 1);
}

static inline wint_t format_next_prev(struct pane *home safe, struct pane *focus safe,
				      struct mark *m safe, struct doc_ref *r safe,
				      int forward, bool bytes)
{
	struct rf_data *rd = &home->data;
	struct rf_field *rf;
	int move = r == &m->ref;
	int f, o;
	int margin;
	int fsize;
	int len = 0;
	const char *val = NULL;
	int index;

	set_format(focus, rd);

	if (!forward) {
		index = normalize(home, focus, m, -1);
		if (index < 0) {
			if (doc_prior(home->parent, m) == WEOF)
				return CHAR_RET(WEOF);
			if (move)
				prev_line(home, m);
			return CHAR_RET('\n');
		}
	} else {
		if (m->ref.p == NULL)
			return CHAR_RET(WEOF);
		index = normalize(home, focus, m, 0);
		if (index < 0)
			/* Should be impossible */
			return CHAR_RET(WEOF);
	}
	f = FIELD_NUM(index);
	o = FIELD_OFFSET(index);

	if (f >= rd->nfields) {
		if (move)
			next_line(home, m);
		return CHAR_RET('\n');
	}
	rf = &rd->fields[f];
	fsize = field_size(home, focus, m, f, &val);
	if (move && forward) {
		index = normalize(home, focus, m, 1);
		if (index < 0) {
			next_line(home, m);
			return CHAR_RET('\n');
		}
		update_offset(m, rd, index);
	} else if (move && !forward) {
		update_offset(m, rd, index);
	}

	if (!rf->var) {
		val = rf->val;
		while (o > 0 && get_utf8(&val, NULL) < WERR) {
			o -= 1;
			if (val[-1] == '%' || val[-1] == '<')
				val += 1;
		}
		return CHAR_RET(get_utf8(&val, NULL));
	}
	if (!val)
		return ' ';

	len = utf8_strlen(val);
	switch (rf->align) {
	case 'l':
	default:
		if (o < len)
			break;
		else
			return ' ';
	case 'c':
		margin = (fsize - len) / 2;
		if (margin < 0)
			margin = 0;
		if (o < margin)
			return ' ';
		if (o >= margin + len)
			return ' ';
		o -= margin;
		break;
	case 'r':
		margin = fsize - len;
		if (margin < 0)
			margin = 0;
		if (o < margin)
			return ' ';
		o -= margin;
		break;
	}
	while (o > 0 && get_utf8(&val, NULL) < WERR)
		o -= 1;
	return CHAR_RET(get_utf8(&val, NULL));
}

DEF_CMD(format_char)
{
	return do_char_byte(ci);
}

DEF_CMD(format_content2)
{
	/* doc:content delivers one char at a time to a callback.
	 * This is used e.g. for 'search' and 'copy'.
	 *
	 * .mark is 'location': to start.  This is moved forwards
	 * .mark if set is a location to stop
	 * .comm2 is 'consume': pass char mark and report if finished.
	 *
	 */
	struct pane *home = ci->home;
	struct pane *focus = ci->focus;
	struct rf_data *rd = &home->data;
	struct rf_field *rf;
	struct mark *m = ci->mark;
	struct mark *end = ci->mark2;
	wint_t nxt, prev;
	int len, index, f, o, fsize, margin;
	const char *val;
	int i;

	if (!m || !ci->comm2)
		return Enoarg;
	if (ci->num)
		/* Cannot handle bytes */
		return Einval;
	set_format(focus, rd);
	m = mark_dup(m);

	pane_set_time(home);
	do {
		if (pane_too_long(home, 2000))
			break;
		if (m->ref.p == NULL)
			break;
		index = normalize(home, focus, m, 0);
		if (index < 0)
			break;

		f = FIELD_NUM(index);
		o = FIELD_OFFSET(index);

		if (f >= rd->nfields) {
			next_line(home, m);
			nxt = '\n';
			continue;
		}
		rf = &rd->fields[f];
		val = NULL;
		fsize = field_size(home, focus, m, f, &val);
		mark_step(m, 1);
		index = normalize(home, focus, m, 1);
		if (index < 0) {
			next_line(home, m);
			nxt = '\n';
			continue;
		}
		update_offset(m, rd, index);

		if (!rf->var) {
			const char *vend = rf->val + rf->val_len;
			prev = WEOF;
			val = rf->val;
			i = 0;
			while ((nxt = get_utf8(&val, vend)) < WERR) {
				if (nxt == '%' || nxt == '<')
					val += 1;
				if (o <= i &&
				    (!end || mark_ordered_or_same(m, end))) {
					if (prev != WEOF) {
						if (comm_call(ci->comm2,
							      "consume", focus,
							      prev, m) <= 0)
							break;
						mark_step(m, 1);
						update_offset(m, rd, MAKE_INDEX(f, i+1));
					}
					prev = nxt;
				}
				i += 1;
			}
			nxt = prev;
			continue;
		}
		if (!val) {
			nxt = ' ';
			continue;
		}
		len = utf8_strlen(val);
		switch (rf->align) {
		case 'l':
		default:
			margin = 0;
			break;
		case 'c':
			margin = (fsize - len) / 2;
			if (margin < 0)
				margin = 0;
			break;
		case 'r':
			margin = fsize - len;
			if (margin < 0)
				margin = 0;
			break;
		}
		prev = nxt = WEOF;
		for (i = 0; i < fsize; i++) {
			if ((rf->align == 'c' &&
			     (i < margin || i >= margin + len)) ||
			    (rf->align == 'r' && i < margin) ||
			     (rf->align != 'c' && rf->align != 'r' &&
			      i >= len))
				nxt = ' ';
			else
				nxt = get_utf8(&val, NULL);
			if (i >= o) {
				if (prev != WEOF) {
					if (comm_call(ci->comm2,
						      "consume", focus,
						      prev, m) <= 0)
						break;
					mark_step(m, 1);
					update_offset(m, rd, MAKE_INDEX(f, i+1));
				}
				prev = nxt;
			}
		}
	} while (nxt > 0 && nxt != WEOF &&
		 (!end || mark_ordered_or_same(m, end)) &&
		 comm_call(ci->comm2, "consume", ci->focus, nxt, m) > 0);

	mark_free(m);
	return 1;
}

DEF_CMD(format_attr)
{
	/* If there are attrs here, we report that by returning
	 * "render:format" as "yes".  This causes map-attr to called so
	 * that it can insert those attrs.
	 *
	 * Also "format:plain" which formats the line directly
	 * without the cost of all the lib-markup machinery.
	 */
	struct rf_data *rd = &ci->home->data;
	struct mark *m = ci->mark;
	int previ;
	int f0, f;
	int idx;
	bool need_attr = False;

	if (!m || !ci->str)
		return Enoarg;
	if (!m->ref.p)
		return Efallthrough;
	if (strcmp(ci->str, "format:plain") == 0) {
		char *v = do_format(ci->focus, m, NULL, -1, 0);

		comm_call(ci->comm2, "", ci->focus, 0, m, v);
		free(v);
	}

	if (ci->num2 == 0 && strcmp(ci->str, "render:format") != 0)
		return Efallthrough;
	if (ci->num2 && strncmp(ci->str, "render:format", strlen(ci->str)) != 0)
		return Efallthrough;

	idx = m->ref.i;
	/* idx of 0 is special and may not be normalized */
	if (idx == 0)
		idx = normalize(ci->home, ci->focus, m, 0);
	if (FIELD_OFFSET(idx) > 0)
		/* attribute changes only happen at start of a field */
		return 1;

	/* There may be several previous fields that are empty.
	 * We need consider the possibility that any of those
	 * change the attributes.
	 */
	previ = normalize(ci->home, ci->focus, m, -1);
	if (previ < 0)
		f0 = 0;
	else
		f0 = FIELD_NUM(previ)+1;
	for(f = f0; f <= FIELD_NUM(idx); f++) {
		if (f < rd->nfields) {
			if (rd->fields[f].attr_end > FIELD_NUM(idx) ||
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
	struct rf_data *rd = &ci->home->data;
	struct mark *m = ci->mark;
	int idx, previ;
	int f0, f;

	if (!m || !ci->str)
		return Enoarg;
	if (strcmp(ci->str, "render:format") != 0)
		return Efallthrough;
	if (m->ref.p == NULL)
		return Efallthrough;
	idx = m->ref.i;
	if (idx == 0)
		idx = normalize(ci->home, ci->focus, m, 0);
	if (FIELD_OFFSET(idx) > 0)
		/* attribute changes only happen at start of a field */
		return 1;
	f = FIELD_NUM(idx);

	/* There may be several previous fields that are empty.
	 * We need to consider the possibility that any of those
	 * change the attributes.
	 */
	previ = normalize(ci->home, ci->focus, m, -1);
	if (previ < 0)
		f0 = 0;
	else
		f0 = FIELD_NUM(previ)+1;
	for(f = f0; f <= FIELD_NUM(idx); f++) {
		if (f >= rd->nfields)
			continue;
		/* Each depth gets a priority level from 0 up.
		 * When starting, set length to v.large.  When ending, set
		 * length to -1.
		 */
		if (rd->fields[f].attr_start < f0) {
			struct rf_field *st =
				&rd->fields[rd->fields[f].attr_start];
			comm_call(ci->comm2, "", ci->focus, -1, m,
				  NULL, st->attr_depth);
		}
		if (rd->fields[f].attr_end > FIELD_NUM(idx)) {
			struct rf_field *st = &rd->fields[f];
			const char *attr = st->attr;
			if (attr && attr[0] == '%')
				attr = pane_mark_attr(ci->focus, m, attr+1);
			comm_call(ci->comm2, "", ci->focus, 0, m,
				  attr, st->attr_depth);
		}
	}
	return 0;
}

DEF_CMD(render_line_prev2)
{
	struct rf_data *rd = &ci->home->data;
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
	update_offset(m, rd, 0);

	return 1;
}

static struct pane *do_render_format_attach(struct pane *parent safe);
DEF_CMD(format_clone)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus);
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

	key_add(rf2_map, "doc:char", &format_char);
	key_add(rf2_map, "doc:get-attr", &format_attr);
	key_add(rf2_map, "map-attr", &format_map);
	key_add(rf2_map, "doc:render-line-prev", &render_line_prev2);
	key_add(rf2_map, "Clone", &format_clone);
	key_add(rf2_map, "doc:content", &format_content2);
	key_add(rf2_map, "Free", &format_free);
	key_add(rf2_map, "doc:shares-ref", &format_noshare_ref);
}

DEF_LOOKUP_CMD(render_format_handle, rf_map);
DEF_LOOKUP_CMD(render_format2_handle, rf2_map);

static struct pane *do_render_format_attach(struct pane *parent safe)
{
	struct pane *p;

	if (call("doc:shares-ref", parent) != 1) {
		if (!rf_map)
			render_format_register_map();

		p = pane_register(parent, 0, &render_format_handle.c, NULL);
	} else {
		if (!rf2_map)
			render_format_register_map();

		p = pane_register(parent, 0, &render_format2_handle.c);
		if (!p)
			return p;

		if (!pane_attr_get(parent, "format:no-linecount")) {
			struct pane *p2 = call_ret(pane, "attach-line-count", p);
			if (p2)
				p = p2;
		}
	}
	if (!p)
		return NULL;
	attr_set_str(&p->attrs, "render-wrap", "no");
	return p;
}

DEF_CMD(render_format_attach)
{
	struct pane *p;

	p = do_render_format_attach(ci->focus);
	if (!p)
		return Efail;
	if (p->handle == &render_format_handle.c)
		p = call_ret(pane, "attach-render-lines", p);
	else
		p = call_ret(pane, "attach-render-text", p);
	if (!p)
		return Efail;
	return comm_call(ci->comm2, "callback:attach", p);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &render_format_attach, 0, NULL, "attach-render-format");
}
