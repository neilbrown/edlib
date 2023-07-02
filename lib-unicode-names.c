/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Parse the Unicode NamesList.txt file to find names for
 * unicode characters.
 */

#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "core.h"

struct unicode_data {
	struct command c;
	char *names;
	int len;
};

static void report_names(struct unicode_data *ud safe, const char *name safe,
			 int which,
			 struct pane *p safe, struct command *c safe)
{
	/* name must be start of a word, as either primary or secondary
	 * name.  Ignore case.
	 * If "which" is zero, return them all, else only return the
	 * nth one where which==n
	 */
	char *ptn = strconcat(p, "?i:^([0-9A-F]{4,5}	|	= ).*\\b", name);
	int i;

	if (!ud->names)
		return;

	for (i = 0; i < ud->len; ) {
		int ch, s;
		char *cp, *n, *eol;

		s = call("text-search", p, 0, NULL, ptn,
			 ud->len - i, NULL, ud->names + i);
		if (s <= 0)
			break;
		i += s-1;
		/* i is now the start of the match */
		cp = ud->names + i;
		eol = strchr(cp, '\n');
		if (!eol)
			break;
		i = (eol - ud->names) + 1;
		if (eol[-1] == '\r')
			eol -= 1;
		if (*cp == '\t') {
			/* secondary name "\t= "*/
			n = strndup(cp+3, eol-cp-3);
			/* find number */
			while (cp > ud->names &&
			       (cp[-1] != '\n' || cp[0] == '\t'))
				cp -= 1;
		} else {
			/* primary name "XXXXX?\t" */
			if (cp[4] == '\t')
				n = strndup(cp+5, eol-cp-5);
			else
				n = strndup(cp+6, eol-cp-6);
		}
		ch = strtoul(cp, &eol, 16);
		if (eol == cp+4 || eol == cp+5) {
			if (which == 0)
				comm_call(c, "cb", p, ch, NULL, n);
			else {
				which -= 1;
				if (which == 0) {
					comm_call(c, "cb", p, ch, NULL, n);
					i = ud->len;
				}
			}
		}
		free(n);
	}
}

static void unicode_free(struct command *c safe)
{
	struct unicode_data *ud = container_of(c, struct unicode_data, c);

	if (ud->names)
		munmap(ud->names, ud->len);
}

DEF_CMD(unicode_names)
{
	struct unicode_data *ud;
	if (ci->comm == &unicode_names) {
		/* This is the first call - need to allocate storage,
		 * load the NamesList file, and register a new command.
		 */
		char *p;
		int fd;

		alloc(ud, pane);
		ud->c = unicode_names;
		ud->c.free = unicode_free;
		call_comm("global-set-command", ci->home, &ud->c, 0, NULL,
			  "Unicode-names");
		p = call_ret(str, "xdg-find-edlib-file", ci->focus, 0, NULL,
			     "NamesList.txt", 0, NULL, "data");
		if (!p)
			return Efail;
		fd = open(p, O_RDONLY);
		free(p);
		if (fd < 0)
			return Efail;
		ud->len = lseek(fd, 0, 2);
		ud->names = mmap(NULL, ud->len, PROT_READ, MAP_SHARED, fd, 0);
		close(fd);
	} else {
		ud = container_of(ci->comm, struct unicode_data, c);
	}
	if (!ud->names)
		return Efail;
	if (ci->str && ci->comm2)
		report_names(ud, ci->str, ci->num, ci->focus, ci->comm2);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &unicode_names,
		  0, NULL, "Unicode-names");
}
