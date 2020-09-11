/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * worddiff - mark word-wise differences in two ranges.
 *
 * ranges currently have to be in same file.
 * We use code from wiggle which requires that a 'stream'
 * (a char[] with length) be split as words into a 'file', then two
 * such 'file's passed to 'diff()' to produce a 'csl' (common subsequence list).
 * The a/b/len of each element in the csl are index to the respective
 * files, which have indexes into the streams.
 */

#include <wctype.h>
#include "core.h"
#include "misc.h"
#include "wiggle/wiggle.h"

/* lib-wiggle currently needs these */
void *xmalloc(int size)
{
	return malloc(size);
}
int do_trace = 0;
void printword(FILE *f, struct elmnt e)
{
}

static void collect(struct pane *p safe, struct mark *start safe,
		    int len, bool skipfirst, struct stream *s safe)
{
	bool sol = True;
	struct mark *m = mark_dup(start);
	wint_t wch;
	struct buf b;

	buf_init(&b);

	while (len--) {
		wch = doc_next(p, m);
		if (wch == WEOF)
			break;
		if (!sol || !skipfirst || is_eol(wch))
			buf_append(&b, wch);
		sol = is_eol(wch);
	}
	s->body = buf_final(&b);
	s->len = b.len;
}

static void add_markup(struct pane *p safe, struct mark *start safe,
		       bool skipfirst, struct stream astream, struct file afile,
		       struct csl *csl safe, const char *attr, int which)
{
	/* Each range of characters that is mentioned in csl gets an attribute
	 * named 'attr' with value 'len' from csl.
	 * If a range crosses a newline, the first (non-skipped) character
	 * also gets the attribute with the remaining length.
	 */
	const char *pos = astream.body;
	bool sol = True;
	struct mark *m = mark_dup(start);
	wint_t ch;

	if (!afile.list)
		return;
	while (csl->len) {
		int st = which ? csl->b : csl->a;
		const char *startp = afile.list[st].start;
		const char *endp =	afile.list[st + csl->len - 1].start +
					afile.list[st + csl->len - 1].len;
		char buf[20];
		int len;

		if (sol && skipfirst) {
			doc_next(p ,m);
			sol = False;
		}
		while (pos < startp) {
			get_utf8(&pos, NULL);
			ch = doc_next(p, m);
			if (skipfirst && is_eol(ch))
				doc_next(p, m);
		}
		/* Convert csl->len in bytes to len in codepoints. */
		len = 0;
		while (pos < endp) {
			get_utf8(&pos, NULL);
			len += 1;
		}
		pos = startp;
		snprintf(buf, sizeof(buf), "%d %d", len, which);
		call("doc:set-attr", p, 0, m, attr, 0, NULL, buf);
		sol = False;
		while (pos < endp) {
			get_utf8(&pos, NULL);
			if (sol) {
				snprintf(buf, sizeof(buf), "%d %d", len, which);
				if (skipfirst)
					ch = doc_next(p, m);
				call("doc:set-attr", p, 0, m, attr,
				     0, NULL, buf);
			}
			len -= 1;
			ch = doc_next(p, m);
			sol = is_eol(ch);
		}
		csl += 1;
	}
}

static bool has_nonspace(const char *s, int len)
{
	wint_t ch;
	const char *end = s+len;

	while ((ch = get_utf8(&s, end)) != WEOF &&
	       ch != WERR &&
	       iswspace(ch))
		;
	return ch != WEOF;
}

static bool only_spaces(struct file f, struct csl *csl safe, int which)
{
	int fpos = 0;

	if (!f.list)
		return True;

	for (; csl->len; csl += 1) {
		int o = which ? csl->b : csl->a;

		for (; fpos < o; fpos += 1) {
			struct elmnt e = f.list[fpos];

			if (has_nonspace(e.start, e.len))
				return False;
		}
		fpos = o + csl->len;
	}
	for (; fpos < f.elcnt; fpos += 1) {
		struct elmnt e = f.list[fpos];

		if (has_nonspace(e.start, e.len))
			return False;
	}
	return True;
}

DEF_CMD(word_diff)
{
	struct pane *p = ci->focus;
	struct mark *astart = ci->mark;
	struct mark *bstart = ci->mark2;
	int alen = ci->num;
	int blen = ci->num2;
	const char *attr = ci->str ?: "render:common";
	bool skipfirst = ci->str2 && ci->str2[0];
	struct stream astream, bstream;
	int ret = Efail;

	if (!astart || !bstart)
		return Enoarg;
	collect(p, astart, alen, skipfirst, &astream);
	collect(p, bstart, blen, skipfirst, &bstream);

	if (astream.len == bstream.len &&
	    memcmp(astream.body, bstream.body, astream.len) == 0)
		/* No difference */
		ret = 1;
	else {
		struct file afile, bfile;
		struct csl *csl;

		afile = split_stream(astream, ByWord);
		bfile = split_stream(bstream, ByWord);
		csl = diff(afile, bfile);
		if (!csl) {
			free(afile.list);
			free(bfile.list);
			goto out;
		}

		add_markup(p, astart, skipfirst, astream, afile, csl, attr, 0);
		add_markup(p, bstart, skipfirst, bstream, bfile, csl, attr, 1);

		ret = 3; /* non-space differences */
		if (only_spaces(afile, csl, 0) &&
		    only_spaces(bfile, csl, 1))
			ret = 2; /* only space differences */

		free(afile.list);
		free(bfile.list);
		free(csl);
	}
out:
	free(astream.body);
	free(bstream.body);
	return ret;
}

static char *typenames[] = {
	[End] = "End",
	[Unmatched] = "Unmatched",
	[Unchanged] = "Unchanged",
	[Extraneous] = "Extraneous",
	[Changed] = "Changed",
	[Conflict] = "Conflict",
	[AlreadyApplied] = "AlreadyApplied",
};

static void add_merge_markup(struct pane *p safe,
			     struct mark *st safe,
			     struct file f, struct merge *merge safe,
			     const char *attr safe, int which)
{
	struct merge *m;
	int pos = 0;

	if (!f.list)
		return;

	for (m = merge; m->type != End; m++) {
		int len;
		const char *cp, *endcp;
		int chars;
		char buf[30];

		switch (which) {
		case 0: /* orig - no Extraneous */
			if (m->type == Extraneous)
				continue;
			if (pos != m->a) abort();
			len = m->al;
			break;
		case 1: /* before - no Unmatched */
			if (m->type == Unmatched)
				continue;
			if (pos != m->b) abort();
			len = m->bl;
			break;
		case 2: /* after - no Unmatched */
			if (m->type == Unmatched)
				continue;
			if (pos != m->c) abort();
			len = m->cl;
			break;
		}
		/* From here for 'len' element in f are 'm->type' */
		if (!len)
			continue;
		cp = f.list[pos].start;
		endcp = f.list[pos+len-1].start + f.list[pos+len-1].len;
		pos += len;
		chars = 0;
		while (get_utf8(&cp, endcp) != WEOF)
			chars += 1;

		snprintf(buf, sizeof(buf), "%d %s", chars, typenames[m->type]);
		call("doc:set-attr", p, 0, st, attr, 0, NULL, buf);
		while (chars > 0) {
			wint_t ch = doc_next(p, st);

			if (ch == WEOF)
				break;
			chars -= 1;
			if (is_eol(ch) && chars > 0) {
				snprintf(buf, sizeof(buf), "%d %s", chars,
					 typenames[m->type]);
				call("doc:set-attr", p, 0, st, attr,
				     0, NULL, buf);
			}
		}
	}
}

DEF_CMD(word_wiggle)
{
	/* 'mark' is one of 6 marks in 'focus' which identify 3
	 * regions, 'orig', 'before' and 'after'.  The marks are
	 * in the same view, so vmark_next() can find all the rest.
	 * Each mark must have attr 'wiggle' with value from
	 * {orig,before,after}-{start,end}
	 * The given mark must be first.
	 *
	 * We extract the texts, run the wiggle algorithm, and then
	 * mark sections of text which are Unmatch, Change, Extranious,
	 * Conflict, AlreadyApplied.  Unchanged are NOT marked.
	 * The attr used is ->str.  Value is "len section-type".
	 */
	struct mark *os = NULL, *oe = NULL;
	struct mark *bs = NULL, *be = NULL;
	struct mark *as = NULL, *ae = NULL;
	struct mark *m;
	struct stream ostr, astr, bstr;
	struct file of, af, bf;
	struct csl *csl1, *csl2;
	struct ci info;

	if (!ci->str)
		return Enoarg;

	for (m = ci->mark; m; m = vmark_next(m)) {
		char *a = attr_find(m->attrs, "wiggle");
		if (!a)
			return Einval;
		if (strcmp(a, "orig-start") == 0)
			os=m;
		if (strcmp(a, "orig-end") == 0)
			oe=m;
		if (strcmp(a, "before-start") == 0)
			bs=m;
		if (strcmp(a, "before-end") == 0)
			be=m;
		if (strcmp(a, "after-start") == 0)
			as=m;
		if (strcmp(a, "after-end") == 0)
			ae=m;
	}
	if (!os || !oe || !bs || !be || !as || !ae)
		return Einval;
	if (oe->seq < os->seq ||
	    be->seq < bs->seq ||
	    ae->seq < as->seq)
		return Einval;

	ostr.body = call_ret(str, "doc:get-str", ci->focus, 0, os, NULL, 0, oe);
	ostr.len = ostr.body ? strlen(ostr.body) : 0;
	bstr.body = call_ret(str, "doc:get-str", ci->focus, 0, bs, NULL, 0, be);
	bstr.len = bstr.body ? strlen(bstr.body) : 0;
	astr.body = call_ret(str, "doc:get-str", ci->focus, 0, as, NULL, 0, ae);
	astr.len = astr.body ? strlen(astr.body) : 0;

	of = split_stream(ostr, ByWord);
	bf = split_stream(bstr, ByWord);
	af = split_stream(astr, ByWord);

	csl1 = diff(of, bf);
	csl2 = diff(bf, af);
	info = make_merger(of, bf, af, csl1, csl2, 1, 0, 0);
	if (info.merger) {
		add_merge_markup(ci->focus, os, of, info.merger, ci->str, 0);
		add_merge_markup(ci->focus, bs, bf, info.merger, ci->str, 1);
		add_merge_markup(ci->focus, as, af, info.merger, ci->str, 2);
	}

	free(csl1);
	free(csl2);
	free(of.list);
	free(bf.list);
	free(af.list);
	free(ostr.body);
	free(bstr.body);
	free(astr.body);

	return info.conflicts + 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &word_diff,
		  0, NULL, "WordDiff");
	call_comm("global-set-command", ed, &word_wiggle,
		  0, NULL, "WordWiggle");
}
