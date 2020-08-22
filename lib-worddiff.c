/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
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

void *xmalloc(int size)
{
	return malloc(size);
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
			do {
				ch = doc_next(p, m);
				sol = is_eol(ch);
			} while (ch != WEOF && skipfirst && sol);
		}
		/* Convert csl->len in bytes to len in codepoints. */
		len = 0;
		while (pos < endp) {
			get_utf8(&pos, NULL);
			len += 1;
		}
		pos = startp;
		sprintf(buf, "%d", len);
		call("doc:set-attr", p, 0, m, attr, 0, NULL, buf);
		while (pos < endp) {
			get_utf8(&pos, NULL);
			len -= 1;
			do {
				if (sol)
					sprintf(buf, "%d", len);
				if (sol && !skipfirst)
					call("doc:set-attr", p, 0, m, attr,
					     0, NULL, buf);
				ch = doc_next(p, m);
				if (sol && skipfirst)
					call("doc:set-attr", p, 0, m, attr,
					     0, NULL, buf);
				sol = is_eol(ch);
			} while (ch != WEOF && skipfirst && sol);
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

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &word_diff,
		  0, NULL, "WordDiff");
}
