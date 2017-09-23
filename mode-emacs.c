/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Define some keystrokes to create an editor with an
 * "emacs" feel.
 *
 * We register an 'emacs' mode and associate keys with that
 * in the global keymap.
 */
#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

#include <stdio.h>

#include "core.h"

static struct map *emacs_map, *hl_map;

REDEF_CMD(emacs_move);
REDEF_CMD(emacs_delete);
REDEF_CMD(emacs_case);
REDEF_CMD(emacs_swap);

static struct move_command {
	struct command	cmd;
	char		*type safe;
	int		direction;
	char		*k1 safe, *k2, *k3;
} move_commands[] = {
	{CMD(emacs_move), "Move-Char", 1,
	 "C-Chr-F", "Right", NULL},
	{CMD(emacs_move), "Move-Char", -1,
	 "C-Chr-B", "Left", NULL},
	{CMD(emacs_move), "Move-Word", 1,
	 "M-Chr-f", "M-Right", NULL},
	{CMD(emacs_move), "Move-Word", -1,
	 "M-Chr-b", "M-Left", NULL},
	{CMD(emacs_move), "Move-WORD", 1,
	 "M-Chr-F", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", -1,
	 "M-Chr-B", NULL, NULL},
	{CMD(emacs_move), "Move-EOL", 1,
	 "C-Chr-E", "End", NULL},
	{CMD(emacs_move), "Move-EOL", -1,
	 "C-Chr-A", "Home", NULL},
	{CMD(emacs_move), "Move-Line", -1,
	 "C-Chr-P", "Up", NULL},
	{CMD(emacs_move), "Move-Line", 1,
	 "C-Chr-N", "Down", NULL},
	{CMD(emacs_move), "Move-File", 1,
	 "M-Chr->", "S-End", NULL},
	{CMD(emacs_move), "Move-File", -1,
	 "M-Chr-<", "S-Home", NULL},
	{CMD(emacs_move), "Move-View-Large", 1,
	 "Next", "C-Chr-V", NULL},
	{CMD(emacs_move), "Move-View-Large", -1,
	 "Prior", "M-Chr-v", NULL},

	{CMD(emacs_delete), "Move-Char", 1,
	 "C-Chr-D", "Del", "del"},
	{CMD(emacs_delete), "Move-Char", -1,
	 "C-Chr-H", "Backspace", NULL},
	{CMD(emacs_delete), "Move-Word", 1,
	 "M-Chr-d", NULL, NULL},
	{CMD(emacs_delete), "Move-Word", -1,
	 "M-C-Chr-H", "M-Backspace", NULL},
	{CMD(emacs_delete), "Move-EOL", 1,
	 "C-Chr-K", NULL, NULL},

	{CMD(emacs_case), "LMove-Word", 1,
	 "M-Chr-l", NULL, NULL},
	{CMD(emacs_case), "UMove-Word", 1,
	 "M-Chr-u", NULL, NULL},
	{CMD(emacs_case), "CMove-Word", 1,
	 "M-Chr-c", NULL, NULL},
	{CMD(emacs_case), "TMove-Char", 1,
	 "M-Chr-`", NULL, NULL},

	{CMD(emacs_swap), "Move-Char", 1,
	 "C-Chr-T", NULL, NULL},
	{CMD(emacs_swap), "Move-Word", 1,
	 "M-Chr-t", NULL, NULL},
};

REDEF_CMD(emacs_move)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct pane *cursor_pane = ci->focus;
	int ret = 0;

	if (!ci->mark)
		return 0;

	ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), ci->mark);
	if (!ret)
		return 0;

	if (strcmp(mv->type, "Move-View-Large") == 0)
		attr_set_int(&cursor_pane->attrs, "emacs-repoint", mv->direction*2);

	return ret;
}

REDEF_CMD(emacs_delete)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *m;

	if (!ci->mark)
		return -1;

	m = mark_dup(ci->mark, 1);

	if (strcmp(mv->type, "Move-EOL") == 0 &&
	    mv->direction == 1 && RPT_NUM(ci) == 1 &&
	    is_eol(doc_following_pane(ci->focus, m)))
		ret = call("Move-Char", ci->focus, mv->direction * RPT_NUM(ci), m);
	else
		ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), m);

	if (!ret) {
		mark_free(m);
		return 0;
	}
	ret = call("Replace", ci->focus, 1, m, NULL, !ci->extra);
	mark_free(m);
	pane_set_extra(ci->focus, 1);

	return ret;
}

REDEF_CMD(emacs_case)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *start = NULL;
	int cnt = mv->direction * RPT_NUM(ci);
	int dir;

	if (!ci->mark)
		return -1;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark, 1);
	}

	while (cnt > 0) {
		struct mark *m = mark_dup(ci->mark, 1);

		ret = call(mv->type+1, ci->focus, dir, ci->mark);
		if (ret <= 0 || mark_same_pane(ci->focus, ci->mark, m))
			/* Hit end of file */
			cnt = 1;
		else {
			char *str = doc_getstr(ci->focus, ci->mark, m);
			char *c;
			int changed = 0;
			int found = 0;

			for (c = str; c && *c; c += 1) {
				char type = mv->type[0];
				if (type == 'C') {
					type = found ? 'L' : 'U';
					if (isalpha(*c))
						found = 1;
				}
				switch(type) {
				default: /* silence compiler */
				case 'U': /* Uppercase */
					if (islower(*c)) {
						changed = 1;
						*c = toupper(*c);
					}
					break;
				case 'L': /* Lowercase */
					if (isupper(*c)) {
						changed = 1;
						*c = tolower(*c);
					}
					break;
				case 'T': /* Toggle */
					if (isupper(*c)) {
						changed = 1;
						*c = tolower(*c);
					} else if (islower(*c)) {
						changed = 1;
						*c = toupper(*c);
					}
					break;
				}
			}
			if (changed) {
				ret = call("Replace", ci->focus, 1, m, str, !ci->extra);
				if (dir < 0)
					call(mv->type+1, ci->focus, dir, ci->mark);
			}
			free(str);
			pane_set_extra(ci->focus, 1);
		}
		mark_free(m);
		cnt -= 1;
	}
	/* When moving forward, move point.  When backward, leave point alone */
	if (start) {
		mark_to_mark(ci->mark, start);
		mark_free(start);
	}
	return ret;
}

REDEF_CMD(emacs_swap)
{
	/* collect the object behind point and insert it after the object
	 * after point
	 */
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *start = NULL;
	int cnt = mv->direction * RPT_NUM(ci);
	int dir;

	if (!ci->mark)
		return -1;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark, 1);
	}

	while (cnt > 0) {
		struct mark *as, *ae, *bs, *be;
		char *astr, *bstr;

		ret = call(mv->type, ci->focus, -dir, ci->mark);
		if (ret <= 0)
			break;
		as = mark_dup(ci->mark, 1);
		ret = call(mv->type, ci->focus, dir, ci->mark);
		if (ret <= 0 || mark_same_pane(ci->focus, ci->mark, as)) {
			mark_free(as);
			break;
		}
		ae = mark_dup(ci->mark, 1);
		call(mv->type, ci->focus, dir, ci->mark);
		be = mark_dup(ci->mark, 1);
		call(mv->type, ci->focus, -dir, ci->mark);
		bs = mark_dup(ci->mark, 1);
		astr = doc_getstr(ci->focus, as, ae);
		bstr = doc_getstr(ci->focus, bs, be);
		mark_to_mark(ci->mark, ae);
		call("Replace", ci->focus, 1, as, bstr, 1);
		mark_to_mark(ci->mark, be);
		call("Replace", ci->focus, 1, bs, astr);
		if (dir < 0)
			call(mv->type, ci->focus, dir, ci->mark);
		free(astr);
		free(bstr);
		mark_free(as);
		mark_free(ae);
		mark_free(bs);
		mark_free(be);
		cnt -= 1;
	}
	/* When moving forward, move point.  When backward, leave point alone */
	if (start) {
		mark_to_mark(ci->mark, start);
		mark_free(start);
	}
	return ret;
}

DEF_CMD(emacs_recenter)
{
	int step = 0;
	if (ci->numeric == NO_NUMERIC && (ci->extra & 2)) {
		/* Repeated command - go to top, or bottom, or middle in order */
		switch (ci->extra & 0xF000) {
		default:
		case 0: /* was center, go to top */
			call("Move-View-Line", ci->focus, 1, ci->mark);
			step = 0x1000;
			break;
		case 0x1000: /* was top, go to bottom */
			call("Move-View-Line", ci->focus, -1, ci->mark);
			step = 0x2000;
			break;
		case 0x2000: /* was bottom, go to middle */
			call("Move-View-Line", ci->focus, 0, ci->mark);
			step = 0;
			break;
		}
	} else if (ci->numeric != NO_NUMERIC) {
		/* Move point to display line N */
		call("Move-View-Line", ci->focus, ci->numeric, ci->mark);
	} else {
		/* Move point to middle and refresh */
		call("Move-View-Line", ci->focus, 0, ci->mark);
		call("Display:refresh", ci->focus);
	}
	call("Mode:set-extra", ci->focus, 0, NULL, NULL, 2| step);
	return 1;
}

REDEF_CMD(emacs_simple);
REDEF_CMD(emacs_simple_neg);
static struct simple_command {
	struct command	cmd;
	char		*type safe;
	char		*k safe;
} simple_commands[] = {
	{CMD(emacs_simple), "Window:next", "emCX-Chr-o"},
	{CMD(emacs_simple), "Window:prev", "emCX-Chr-O"},
	{CMD(emacs_simple), "Window:x+", "emCX-Chr-}"},
	{CMD(emacs_simple), "Window:x-", "emCX-Chr-{"},
	{CMD(emacs_simple), "Window:y+", "emCX-Chr-^"},
	{CMD(emacs_simple), "Window:close-others", "emCX-Chr-1"},
	{CMD(emacs_simple), "Window:split-y", "emCX-Chr-2"},
	{CMD(emacs_simple), "Window:split-x", "emCX-Chr-3"},
	{CMD(emacs_simple), "Window:close", "emCX-Chr-0"},
	{CMD(emacs_simple), "Window:scale-relative", "emCX-C-Chr-="},
	{CMD(emacs_simple_neg), "Window:scale-relative", "emCX-C-Chr--"},
	{CMD(emacs_simple), "Display:new", "emCX5-Chr-2"},
	{CMD(emacs_simple), "Abort", "C-Chr-G"},
	{CMD(emacs_simple), "NOP", "M-Chr-G"},
	{CMD(emacs_simple), "NOP", "emCX-C-Chr-G"},
	{CMD(emacs_simple), "NOP", "emCX4-C-Chr-G"},
	{CMD(emacs_simple), "doc:save-file", "emCX-C-Chr-S"},
};

REDEF_CMD(emacs_simple)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return -1;

	return call(sc->type, ci->focus, ci->numeric, ci->mark, NULL, ci->extra);
}

REDEF_CMD(emacs_simple_neg)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return -1;

	return call(sc->type, ci->focus, -RPT_NUM(ci), ci->mark, NULL, ci->extra);
}

DEF_CMD(emacs_exit)
{
	if (ci->numeric == NO_NUMERIC) {
		struct pane *p = call_pane("PopupTile", ci->focus, 0, NULL, "DM");
		// FIXME if called from a popup, this fails.
		if (!p)
			return 0;
		attr_set_str(&p->attrs, "done-key", "event:deactivate");
		return call("docs:show-modified", p);
	} else
		call("event:deactivate", ci->focus);
	return 1;
}

DEF_CMD(emacs_insert)
{
	int ret;
	char *str;

	if (!ci->mark)
		return -1;

	/* Key is "Chr-X" - skip 4 bytes to get X */
	str = ci->key + 4;
	ret = call("Replace", ci->focus, 1, ci->mark, str, !ci->extra);
	pane_set_extra(ci->focus, 1);

	return ret;
}

static struct {
	char *key;
	char *insert;
} other_inserts[] = {
	{"Tab", "\t"},
	{"LF", "\n"},
	{"Return", "\n"},
	{"C-Chr-O", "\0\n"},
	{NULL, NULL}
};

DEF_CMD(emacs_insert_other)
{
	int ret;
	int i;
	struct mark *m = NULL;
	char *ins;

	if (!ci->mark)
		return -1;

	for (i = 0; other_inserts[i].key; i++)
		if (strcmp(safe_cast other_inserts[i].key, ci->key) == 0)
			break;
	ins = other_inserts[i].insert;
	if (ins == NULL)
		return 0;

	if (!*ins) {
		ins++;
		m = mark_dup(ci->mark, 1);
		if (m->seq > ci->mark->seq)
			/* Move m before ci->mark, so it doesn't move when we insert */
			mark_to_mark(m, ci->mark);
	}

	ret = call("Replace", ci->focus, 1, ci->mark, ins, !ci->extra);
	if (m) {
		mark_to_mark(ci->mark, m);
		mark_free(m);
	}
	pane_set_extra(ci->focus, 0); /* A newline starts a new undo */
	return ret;
}

DEF_CMD(emacs_undo)
{
	doc_undo(ci->focus, 0);
	return 1;
}

DEF_CMD(emacs_redo)
{
	doc_undo(ci->focus, 1);
	return 1;
}

REDEF_CMD(emacs_file_complete);
REDEF_CMD(emacs_doc_complete);

DEF_CMD(find_complete)
{
	char *type = ci->home->data;
	if (strcmp(type, "cmd") == 0)
		return 0;
	if (strcmp(type, "file") == 0)
		return emacs_file_complete_func(ci);
	else
		return emacs_doc_complete_func(ci);
}

DEF_CMD(find_done)
{
	int ret;
	char *str = doc_getstr(ci->focus, NULL, NULL);

	ret = call("popup:close", ci->focus, 0, NULL, str);
	free(str);
	return ret;
}

static struct
map *fh_map;
static void findmap_init(void)
{
	fh_map = key_alloc();
	key_add(fh_map, "Tab", &find_complete);
	key_add(fh_map, "Return", &find_done);
}

DEF_LOOKUP_CMD(find_handle, fh_map);

DEF_CMD(emacs_findfile)
{
	int fd;
	struct pane *p, *par;

	if (strncmp(ci->key, "File Found", 10) != 0) {
		char *path = NULL;
		char buf[PATH_MAX];
		char *e;

		path = pane_attr_get(ci->focus, "filename");
		if (path) {
			strcpy(buf, path);
			path = strrchr(buf, '/');
			if (path)
				path[1] = '\0';
			path = buf;
		}

		if (!path)
			path = realpath(".", buf);

		if (!path) {
			strcpy(buf, "/");
			path = buf;
		}
		e = buf + strlen(buf);
		if (e < buf + sizeof(buf)-1 && e > buf && e[-1] != '/') {
			*e++ = '/';
			*e++ = '\0';
		}
		p = call_pane("PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, path);
		if (!p)
			return 0;

		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prefix",
				     "Find File Other Window: ");
			attr_set_str(&p->attrs, "done-key",
				     "File Found Other Window");
		} else {
			attr_set_str(&p->attrs, "prefix", "Find File: ");
			attr_set_str(&p->attrs, "done-key", "File Found");
		}
		call("doc:set-name", p, 0, NULL, "Find File");

		pane_register(p, 0, &find_handle.c, "file", NULL);
		return 1;
	}

	if (strcmp(ci->key, "File Found Other Window") == 0)
		par = call_pane("OtherPane", ci->focus);
	else
		par = call_pane("ThisPane", ci->focus);

	if (!par)
		return -1;

	fd = open(ci->str, O_RDONLY);
	if (fd >= 0) {
		p = call_pane("doc:open", ci->focus, fd, NULL, ci->str);
		close(fd);
	} else
		p = call_pane("doc:open", ci->focus, -2, NULL, ci->str);
	if (p)
		doc_attach_view(par, p, NULL);
	if (!p)
		return -1;
	pane_focus(p);
	return 1;
}

DEF_CMD(save_str)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);
	cr->s = ci->str ? strdup(ci->str) : NULL;
	return 1;
}

REDEF_CMD(emacs_file_complete)
{
	/* Extract a directory name and a basename from the document.
	 * Find a document for the directory and attach as a completing
	 * popup menu
	 */
	char *str;
	char *d, *b, *c;
	int fd;
	struct pane *par, *pop, *docp, *p;
	struct call_return cr;
	int ret;

	if (!ci->mark)
		return -1;

	str = doc_getstr(ci->focus, NULL, NULL);
	if (!str)
		return -1;
	d = str;
	while ((c = strstr(d, "//")) != NULL)
		d = c+1;
	b = strrchr(d, '/');
	if (b) {
		b += 1;
		d = strndup(d, b-d);
	} else {
		b = d;
		d = strdup(".");
	}
	fd = open(d, O_DIRECTORY|O_RDONLY);
	if (fd < 0) {
		free(d);
		free(str);
		return -1;
	}
	docp = call_pane("doc:open", ci->focus, fd, NULL, d);
	close(fd);
	if (!docp)
		return -1;
	pop = call_pane("PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		return -1;
	par = doc_attach_view(pop, docp, NULL);
	if (!par)
		return -1;

	attr_set_str(&par->attrs, "line-format", "%+name%suffix");
	attr_set_str(&par->attrs, "heading", "");
	attr_set_str(&par->attrs, "done-key", "Replace");
	p = render_attach("complete", par);
	if (!p)
		return -1;
	cr.c = save_str;
	cr.s = NULL;
	ret = call_comm("Complete:prefix", p, 0, NULL, b, &cr.c);
	free(d);
	if (cr.s && (strlen(cr.s) <= strlen(b) && ret-1 > 1)) {
		/* We need the dropdown */
		pane_damaged(par, DAMAGED_CONTENT);
		free(str);
		free(cr.s);
		return 1;
	}
	if (cr.s) {
		/* add the extra chars from ci2.str */
		c = cr.s + strlen(b);

		call("Replace", ci->focus, 1, ci->mark, c);
		free(cr.s);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_finddoc)
{
	struct pane *p, *par;

	if (strncmp(ci->key, "Doc Found", 9) != 0) {

		p = call_pane("PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
		if (!p)
			return 0;

		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prefix",
				     "Find Document Other Window: ");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found Other Window");
		} else {
			attr_set_str(&p->attrs, "prefix", "Find Document: ");
			attr_set_str(&p->attrs, "done-key", "Doc Found");
		}
		call("doc:set-name", p, 0, NULL, "Find Document");

		pane_register(p, 0, &find_handle.c, "doc", NULL);
		return 1;
	}

	p = call_pane("docs:byname", ci->focus, 0, NULL, ci->str);
	if (!p)
		return -1;

	if (strcmp(ci->key, "Doc Found Other Window") == 0)
		par = call_pane("OtherPane", ci->focus);
	else
		par = call_pane("ThisPane", ci->focus);
	if (!p || !par)
		return -1;

	p = doc_attach_view(par, p, NULL);
	return !!p;
}

REDEF_CMD(emacs_doc_complete)
{
	/* Extract a document from the document.
	 * Attach the 'docs' document as a completing popup menu
	 */
	char *str;
	struct pane *par, *pop, *docs, *p;
	struct call_return cr;
	int ret;

	if (!ci->mark)
		return -1;

	str = doc_getstr(ci->focus, NULL, NULL);
	if (!str)
		return -1;
	pop = call_pane("PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		return -1;
	docs = call_pane("docs:byname", ci->focus);
	if (!docs)
		return -1;
	par = doc_attach_view(pop, docs, NULL);
	if (!par)
		return -1;

	attr_set_str(&par->attrs, "line-format", "%+name");
	attr_set_str(&par->attrs, "heading", "");
	attr_set_str(&par->attrs, "done-key", "Replace");
	p = render_attach("complete", par);
	if (!p)
		return -1;
	cr.c = save_str;
	cr.s = NULL;
	ret = call_comm("Complete:prefix", p, 0, NULL, str, &cr.c);
	if (cr.s && (strlen(cr.s) <= strlen(str) && ret - 1 > 1)) {
		/* We need the dropdown */
		pane_damaged(par, DAMAGED_CONTENT);
		free(str);
		free(cr.s);
		return 1;
	}
	if (cr.s) {
		/* add the extra chars from cr.s */
		char *c = cr.s + strlen(str);

		call("Replace", ci->focus, 1, ci->mark, c);
		free(cr.s);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_viewdocs)
{
	struct pane *p, *par;
	struct pane *docs;

	docs = call_pane("docs:byname", ci->focus, 0, NULL, "*Documents*");
	if (!docs)
		return -1;
	par = call_pane("ThisPane", ci->focus);
	if (!par)
		return -1;

	p = doc_attach_view(par, docs, NULL);
	return !!p;
}

DEF_CMD(emacs_shell)
{
	char *name = "*Shell Command Output*";
	struct pane *p, *doc, *par;
	char *path;

	if (strcmp(ci->key, "Shell Command") != 0) {
		p = call_pane("PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
		if (!p)
			return 0;
		attr_set_str(&p->attrs, "prefix", "Shell command: ");
		attr_set_str(&p->attrs, "done-key", "Shell Command");
		call("doc:set-name", p, 0, NULL, "Shell Command");
		p = call_pane("attach-history", p, 0, NULL, "*Shell History*",
			      0, NULL, "popup:close");
		pane_register(p, 0, &find_handle.c, "cmd", NULL);
		return 1;
	}
	path = pane_attr_get(ci->focus, "filename");
	if (path) {
		char *e = strrchr(path, '/');
		if (e && e > path)
			*e = 0;
	}
	par = call_pane("OtherPane", ci->focus);
	if (!par)
		return -1;
	/* Find or create "*Shell Command Output*" */
	doc = call_pane("docs:byname", ci->focus, 0, NULL, name);
	if (!doc)
		doc = call_pane("doc:from-text", par, 0, NULL, name, 0, NULL, "");
	if (!doc)
		return -1;
	p = call_pane("doc:attach", doc);
	if (!p)
		return -1;
	call_home(p, "doc:assign", doc);
	call_pane("attach-shellcmd", p, 0, NULL, ci->str, 0, NULL, path);
	doc_attach_view(par, doc, "default:viewer");
	return 1;
}

DEF_CMD(emacs_meta)
{
	pane_set_mode(ci->focus, "M-");
	pane_set_numeric(ci->focus, ci->numeric);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}

DEF_CMD(emacs_num)
{
	int rpt = ci->numeric;
	char *last = ci->key + strlen(ci->key)-1;
	int neg = 0;

	if (rpt < 0) {
		neg = 1;
		rpt = -rpt;
	}
	if (rpt == NO_NUMERIC)
		rpt = 0;

	rpt = rpt * 10 + *last - '0';

	pane_set_numeric(ci->focus, neg ? -rpt : rpt);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}

DEF_CMD(emacs_neg)
{
	pane_set_numeric(ci->focus, - ci->numeric);
	pane_set_extra(ci->focus, ci->extra);
	return 1;
}

DEF_CMD(emacs_kill_doc)
{
	return call("doc:destroy", ci->focus);
}

DEF_CMD(emacs_save_all)
{
	if (ci->numeric == NO_NUMERIC) {
		struct pane *p = call_pane("PopupTile", ci->focus, 0, NULL, "DM");
		if (p)
			return call("docs:show-modified", p);
	}
	return call("docs:save-all", ci->focus);
}

static void do_searches(struct pane *p safe, int view, char *patn,
			struct mark *m, struct mark *end)
{
	int ret;
	if (!m)
		return;
	m = mark_dup(m, 1);
	while ((ret = call("text-search", p, 0, m, patn, 0, end)) >= 1) {
		struct mark *m2, *m3;
		int len = ret - 1;
		m2 = vmark_new(p, view);
		if (!m2)
			break;
		mark_to_mark(m2, m);
		while (ret > 1 && mark_prev_pane(p, m2) != WEOF)
			ret -= 1;
		m3 = vmark_matching(p, m2);
		if (m3) {
			mark_free(m2);
			m2 = m3;
		}
		if (attr_find(m2->attrs, "render:search") == NULL)
			attr_set_int(&m2->attrs, "render:search2", len);
	}
	mark_free(m);
}

struct highlight_info {
	int view;
	char *patn;
	struct pane *popup safe;
};

DEF_CMD(emacs_search_highlight)
{
	/* from 'mark' for 'numeric' chars there is a match for 'str' */
	struct mark *m, *start, *end;
	struct highlight_info *hi = ci->home->data;

	if (hi->view <= 0)
		return 0;

	start = vmark_first(ci->focus, hi->view);
	end = vmark_last(ci->focus, hi->view);
	while (start && (m = vmark_next(start)) != NULL && m != end)
		mark_free(m);
	if (start) {
		attr_set_str(&start->attrs, "render:search", NULL);
		attr_set_str(&start->attrs, "render:search2", NULL);
	}
	if (end) {
		attr_set_str(&end->attrs, "render:search", NULL);
		attr_set_str(&end->attrs, "render:search2", NULL);
	}

	free(hi->patn);
	hi->patn = NULL;

	if (ci->mark && ci->numeric > 0 && ci->str) {
		hi->patn = strdup(ci->str);
		m = vmark_new(ci->focus, hi->view);
		if (!m)
			return -1;
		mark_to_mark(m, ci->mark);
		attr_set_int(&m->attrs, "render:search", ci->numeric);
		call("Move-View-Pos", ci->focus, 0, m);
		call("Notify:doc:Replace", ci->focus);
		if (start) {
			m = mark_dup(start, 1);
			do_searches(ci->focus, hi->view, ci->str,
				    m, end);
			mark_free(m);
		}
	} else
		call("Notify:doc:Replace", ci->focus);
	pane_damaged(ci->home, DAMAGED_CONTENT|DAMAGED_VIEW);
	return 1;
}

DEF_CMD(highlight_draw)
{
	struct highlight_info *hi = ci->home->data;
	struct pane *pp = hi->popup;

	if (!ci->str2 || !strstr(ci->str2, ",focus"))
		return 0;

	/* here is where the user will be looking, make sure
	 * the popup doesn't obscure it.
	 */

	while (pp->parent && pp->z == 0)
		pp = pp->parent;
	if (pp->x == 0) {
		/* currently TL, should we move it back */
		if (ci->y > pp->h || ci->x < pp->w)
			call("popup:style", hi->popup, 0, NULL, "TR2");
	} else {
		/* currently TR, should we move it out of way */
		if (ci->y <= pp->h && ci->x >= pp->x)
			call("popup:style", hi->popup, 0, NULL, "TL2");
	}
	return 0;
}

DEF_CMD(emacs_reposition)
{
	struct mark *m;
	int repoint = attr_find_int(ci->focus->attrs, "emacs-repoint");

	if (repoint != -1) {
		/* Move point to end of display, if that is in
		 * the right direction.  That will mean it have moved
		 * off the display.
		 */
		m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
		if (m) {
			struct mark *m2 = mark_dup(m ,1);
			call("Mouse-event", ci->focus, 1, m, "Move-CursorXY",
			     0, NULL, NULL, NULL,
			     -1, repoint < 0 ? ci->focus->h-1 : 0);
			if (repoint < 0)
				/* can only move point backwards */
				if (m->seq < m2->seq)
					call("Move-to", ci->focus, 0, m);
			if (repoint > 0)
				/* can only move point forwards */
				if (m->seq > m2->seq)
					call("Move-to", ci->focus, 0, m);
			mark_free(m);
			mark_free(m2);
		}
		attr_set_str(&ci->focus->attrs, "emacs-repoint", NULL);
	}
	return 0;
}

DEF_CMD(emacs_search_reposition)
{
	/* If new range and old range don't over-lap, discard
	 * old range and re-fill new range.
	 * Otherwise delete anything in range that is no longer visible.
	 * If they overlap before, search from start to first match.
	 * If they overlap after, search from last match to end.
	 */
	/* delete every match before new start and after end */
	struct highlight_info *hi = ci->home->data;
	struct mark *start = ci->mark;
	struct mark *end = ci->mark2;
	struct mark *vstart, *vend;
	char *patn = hi->patn;
	int damage = 0;
	struct mark *m;

	if (hi->view < 0 || patn == NULL || !start || !end)
		return 0;

	while ((m = vmark_first(ci->focus, hi->view)) != NULL &&
	       m->seq < start->seq) {
		mark_free(m);
		damage = 1;
	}
	while ((m = vmark_last(ci->focus, hi->view)) != NULL &&
	       m->seq > end->seq){
		mark_free(m);
		damage = 1;
	}

	vstart = vmark_first(ci->focus, hi->view);
	vend = vmark_last(ci->focus, hi->view);
	if (vstart == NULL || start->seq < vstart->seq) {
		/* search from 'start' to first match or 'end' */
		do_searches(ci->focus, hi->view, patn, start, vstart ?: end);
		if (vend)
			do_searches(ci->focus, hi->view, patn,
				    vend, end);
	} else if (vend && end->seq > vend->seq) {
		/* search from last match to end */
		do_searches(ci->focus, hi->view, patn, vend, end);
	}
	if (vstart != vmark_first(ci->focus, hi->view) ||
	    vend != vmark_last(ci->focus, hi->view))
		damage = 1;

	if (damage)
		pane_damaged(ci->focus, DAMAGED_CONTENT|DAMAGED_VIEW);
	return 0;
}

DEF_LOOKUP_CMD(highlight_handle, hl_map);

DEF_CMD(emacs_start_search)
{
	struct pane *p, *hp;
	struct highlight_info *hi = calloc(1, sizeof(*hi));

	hi->view = doc_add_view(ci->focus);
	hp = pane_register(ci->focus, 0, &highlight_handle.c, hi, NULL);

	p = call_pane("PopupTile", hp, 0, NULL, "TR2", 0, NULL, "");

	if (!p)
		return 0;
	hi->popup = p;

	attr_set_str(&p->attrs, "prefix", "Search: ");
	attr_set_str(&p->attrs, "done-key", "Search String");
	call("doc:set-name", p, 0, NULL, "Search");
	call_pane("attach-emacs-search", p, ci->key[6] == 'R');

	return 1;
}

DEF_CMD(emacs_highlight_close)
{
	/* ci->focus is being closed */
	struct highlight_info *hi = ci->home->data;

	free(hi->patn);
	if (hi->view >= 0) {
		struct mark *m;

		while ((m = vmark_first(ci->focus, hi->view)) != NULL)
			mark_free(m);
		doc_del_view(ci->focus, hi->view);
	}
	free(hi);
	return 0;
}

DEF_CMD(emacs_search_done)
{
	if (ci->str && ci->str[0]) {
		call("global-set-attr", ci->focus, 0, NULL, "Search String",
		     0, NULL, ci->str);
	}
	pane_close(ci->home);
	return 1;
}

DEF_CMD(emacs_bury)
{
	/* Display something else in this tile. */
	struct pane *tile, *doc;
	tile = call_pane("ThisPane", ci->focus);
	if (!tile)
		return 1;
	call("doc:revisit", ci->focus, -1);
	doc = call_pane("docs:choose", ci->focus);
	if (doc)
		doc_attach_view(tile, doc, NULL);
	return 1;
}

DEF_CMD(emacs_command)
{
	struct pane *p;

	p = call_pane("PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
	if (!p)
		return 0;
	attr_set_str(&p->attrs, "prefix", "Cmd: ");
	attr_set_str(&p->attrs, "done-key", "emacs:command");
	call("doc:set-name", p, 0, NULL, "M-x command");
	pane_register(p, 0, &find_handle.c, "file", NULL);
	return 1;
}

DEF_CMD(emacs_do_command)
{
	char cmd[30];
	int ret;

	snprintf(cmd, sizeof(cmd), "interactive-cmd-%s", ci->str);
	ret = call(cmd, ci->focus, 0, ci->mark, ci->str);
	if (ret == 0) {
		snprintf(cmd, sizeof(cmd), "Command %s not found", ci->str);
		call("Message", ci->focus, 0, NULL, cmd);
	} else if (ret < 0) {
		snprintf(cmd, sizeof(cmd), "Command %s Failed", ci->str);
		call("Message", ci->focus, 0, NULL, cmd);
	}
	return 1;
}

DEF_CMD(emacs_version)
{
	call("Message", ci->focus, 0, NULL, "Version: edlib-0.0-devel");
	return 1;
}

DEF_CMD(emacs_attrs)
{
	struct highlight_info *hi = ci->home->data;

	if (!ci->str)
		return 0;

	if (strcmp(ci->str, "render:search") == 0) {
		/* Current search match -  "20" is a priority */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int  len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:red,inverse,focus", 20);
		}
	}
	if (strcmp(ci->str, "render:search2") == 0) {
		/* alternate matches in current view */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:blue,inverse", 20);
		}
	}

	return 0;
}

static void emacs_init(void)
{
	unsigned i;
	struct command *cx_cmd = key_register_prefix("emCX-");
	struct command *cx4_cmd = key_register_prefix("emCX4-");
	struct command *cx5_cmd = key_register_prefix("emCX5-");
	struct map *m = key_alloc();

	key_add(m, "C-Chr-X", cx_cmd);
	key_add(m, "emCX-Chr-4", cx4_cmd);
	key_add(m, "emCX-Chr-5", cx5_cmd);
	key_add(m, "ESC", &emacs_meta);

	for (i = 0; i < ARRAY_SIZE(move_commands); i++) {
		struct move_command *mc = &move_commands[i];
		key_add(m, mc->k1, &mc->cmd);
		if (mc->k2)
			key_add(m, mc->k2, &mc->cmd);
		if (mc->k3)
			key_add(m, mc->k3, &mc->cmd);
	}

	for (i = 0; i < ARRAY_SIZE(simple_commands); i++) {
		struct simple_command *sc = &simple_commands[i];
		key_add(m, sc->k, &sc->cmd);
	}

	key_add_range(m, "Chr- ", "Chr-~", &emacs_insert);
	key_add_range(m, "Chr-\200", "Chr-\377\377\377\377", &emacs_insert);
	key_add(m, "Tab", &emacs_insert_other);
	key_add(m, "LF", &emacs_insert_other);
	key_add(m, "Return", &emacs_insert_other);
	key_add(m, "C-Chr-O", &emacs_insert_other);

	key_add(m, "C-Chr-_", &emacs_undo);
	key_add(m, "M-C-Chr-_", &emacs_redo);

	key_add(m, "C-Chr-L", &emacs_recenter);

	key_add(m, "emCX-C-Chr-F", &emacs_findfile);
	key_add(m, "emCX4-C-Chr-F", &emacs_findfile);
	key_add(m, "emCX4-Chr-f", &emacs_findfile);
	key_add(m, "File Found", &emacs_findfile);
	key_add(m, "File Found Other Window", &emacs_findfile);

	key_add(m, "emCX-Chr-b", &emacs_finddoc);
	key_add(m, "emCX4-Chr-b", &emacs_finddoc);
	key_add(m, "Doc Found", &emacs_finddoc);
	key_add(m, "Doc Found Other Window", &emacs_finddoc);
	key_add(m, "emCX-C-Chr-B", &emacs_viewdocs);

	key_add(m, "emCX-Chr-k", &emacs_kill_doc);

	key_add(m, "emCX-Chr-s", &emacs_save_all);

	key_add(m, "C-Chr-S", &emacs_start_search);
	key_add(m, "C-Chr-R", &emacs_start_search);
	key_add(m, "render:reposition", &emacs_reposition);

	key_add(m, "emCX-C-Chr-C", &emacs_exit);

	key_add(m, "M-Chr-!", &emacs_shell);
	key_add(m, "Shell Command", &emacs_shell);

	key_add(m, "M-Chr-B", &emacs_bury);

	key_add_range(m, "M-Chr-0", "M-Chr-9", &emacs_num);
	key_add(m, "M-Chr--", &emacs_neg);

	key_add(m, "M-Chr-x", &emacs_command);
	key_add(m, "emacs:command", &emacs_do_command);
	key_add(m, "interactive-cmd-version", &emacs_version);

	emacs_map = m;

	m = key_alloc();
	key_add(m, "Search String", &emacs_search_done);
	key_add(m, "render:reposition", &emacs_search_reposition);
	key_add(m, "search:highlight", &emacs_search_highlight);
	key_add(m, "map-attr", &emacs_attrs);
	key_add(m, "Draw:text", &highlight_draw);
	key_add(m, "Close", &emacs_highlight_close);
	hl_map = m;
}

DEF_LOOKUP_CMD(mode_emacs, emacs_map);

DEF_CMD(attach_mode_emacs)
{
	return call_comm("global-set-keymap", ci->focus, &mode_emacs.c);
}

void emacs_search_init(struct pane *ed safe);
void edlib_init(struct pane *ed safe)
{
	if (emacs_map == NULL)
		emacs_init();
	if (fh_map == NULL)
		findmap_init();
	call_comm("global-set-command", ed, 0, NULL, "attach-mode-emacs",
		  &attach_mode_emacs);
	emacs_search_init(ed);
}
