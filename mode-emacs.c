/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Define some keystrokes to create an editor with an
 * "emacs" feel.
 *
 * We register an 'emacs' mode and associate keys with that
 * in the global keymap.
 */
#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <wchar.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>

#include <stdio.h>

#include "core.h"

static struct map *emacs_map, *hl_map;
static char * safe file_normalize(struct pane *p safe, char *path,
				  char *initial_path);

/* num2 is used to track if successive commands are related.
 * Only low 16 bits identify command, other bits are free.
 */
enum {
	N2_zero = 0,
	N2_undo_insert,	/* adjacent commands form a single undo set */
	N2_undo_delete,	/* adjacent commands form a single undo set */
	N2_undo_change,	/* adjacent commands form a single undo set */
	N2_recentre,	/* repeated recentre goes to different places */
	N2_yank,	/* repeated yank-pop takes different result */
	N2_match,	/* repeated ` after Cx` repeats the search */
	N2_undo,	/* Last command was 'undo' too */
};
static inline int N2(const struct cmd_info *ci safe)
{
	return ci->num2 & 0xffff;
}

static inline int N2a(const struct cmd_info *ci safe)
{
	return ci->num2 >> 16;
}

REDEF_CMD(emacs_move);
REDEF_CMD(emacs_delete);
REDEF_CMD(emacs_kill);
REDEF_CMD(emacs_case);
REDEF_CMD(emacs_swap);

static struct move_command {
	struct command	cmd;
	char		*type safe;
	int		direction, extra;
	char		*k1 safe, *k2, *k3;
} move_commands[] = {
	{CMD(emacs_move), "Move-Char", 1, 0,
	 "C-Chr-F", "Right", NULL},
	{CMD(emacs_move), "Move-Char", -1, 0,
	 "C-Chr-B", "Left", NULL},
	{CMD(emacs_move), "Move-Word", 1, 0,
	 "M-Chr-f", "M-Right", NULL},
	{CMD(emacs_move), "Move-Word", -1, 0,
	 "M-Chr-b", "M-Left", NULL},
	{CMD(emacs_move), "Move-Expr", 1, 0,
	 "M-C-Chr-F", NULL, NULL},
	{CMD(emacs_move), "Move-Expr", -1, 0,
	 "M-C-Chr-B", NULL, NULL},
	{CMD(emacs_move), "Move-Expr", -1, 1,
	 "M-C-Chr-U", NULL, NULL},
	{CMD(emacs_move), "Move-Expr", 1, 1,
	 "M-C-Chr-D", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", 1, 0,
	 "M-Chr-F", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", -1, 0,
	 "M-Chr-B", NULL, NULL},
	{CMD(emacs_move), "Move-EOL", 1, 0,
	 "C-Chr-E", "End", NULL},
	{CMD(emacs_move), "Move-EOL", -1, 0,
	 "C-Chr-A", "Home", NULL},
	{CMD(emacs_move), "Move-Line", -1, 0,
	 "C-Chr-P", "Up", NULL},
	{CMD(emacs_move), "Move-Line", 1, 0,
	 "C-Chr-N", "Down", NULL},
	{CMD(emacs_move), "Move-File", 1, 0,
	 "M-Chr->", "S-End", NULL},
	{CMD(emacs_move), "Move-File", -1, 0,
	 "M-Chr-<", "S-Home", NULL},
	{CMD(emacs_move), "Move-View-Large", 1, 0,
	 "Next", "C-Chr-V", "emacs-move-large-other"},
	{CMD(emacs_move), "Move-View-Large", -1, 0,
	 "Prior", "M-Chr-v", NULL},

	{CMD(emacs_move), "Move-Paragraph", -1, 0,
	 "M-C-Chr-A", NULL, NULL},
	{CMD(emacs_move), "Move-Paragraph", 1, 0,
	 "M-C-Chr-E", NULL, NULL},

	{CMD(emacs_delete), "Move-Char", 1, 0,
	 "C-Chr-D", "Del", "del"},
	{CMD(emacs_delete), "Move-Char", -1, 0,
	 "C-Chr-H", "Backspace", "Delete"},
	{CMD(emacs_delete), "Move-Word", 1, 0,
	 "M-Chr-d", NULL, NULL},
	{CMD(emacs_delete), "Move-Word", -1, 0,
	 "M-C-Chr-H", "M-Backspace", NULL},
	{CMD(emacs_kill), "Move-EOL", 1, 0,
	 "C-Chr-K", NULL, NULL},
	{CMD(emacs_kill), "Move-Expr", 1, 0,
	 "M-C-Chr-K", NULL, NULL},

	{CMD(emacs_case), "LMove-Word", 1, 0,
	 "M-Chr-l", NULL, NULL},
	{CMD(emacs_case), "UMove-Word", 1, 0,
	 "M-Chr-u", NULL, NULL},
	{CMD(emacs_case), "CMove-Word", 1, 0,
	 "M-Chr-c", NULL, NULL},
	{CMD(emacs_case), "TMove-Char", 1, 0,
	 "M-Chr-`", NULL, NULL},

	{CMD(emacs_swap), "Move-Char", 1, 0,
	 "C-Chr-T", NULL, NULL},
	{CMD(emacs_swap), "Move-Word", 1, 0,
	 "M-Chr-t", NULL, NULL},
};

REDEF_CMD(emacs_move)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	struct pane *cursor_pane = ci->focus;
	int ret = 0;
	struct mark *mk;

	if (!ci->mark)
		return 0;

	/* if Move-file, leave inactive mark behind */
	if (strcmp(mv->type, "Move-File") == 0) {
		mk = call_ret(mark2, "doc:point", ci->focus);
		if (mk)
			/* Don't change emacs:active */
			mark_to_mark(mk, ci->mark);
		else {
			call("Move-to", ci->focus, 1, ci->mark);
			mk = call_ret(mark2, "doc:point", ci->focus);

			if (mk)
				attr_set_int(&mk->attrs, "emacs:active", 0);
		}
	}

	ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), ci->mark,
		   NULL, mv->extra);
	if (!ret)
		return 0;

	if (strcmp(mv->type, "Move-View-Large") == 0)
		attr_set_int(&cursor_pane->attrs, "emacs-repoint",
			     mv->direction*2);

	mk = call_ret(mark2, "doc:point", ci->focus);
	if (mk && attr_find_int(mk->attrs, "emacs:active") == 2) {
		/* Transient highlight - discard it */
		struct mark *p = call_ret(mark, "doc:point", ci->focus);
		attr_set_int(&mk->attrs, "emacs:active", 0);
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
	}

	return ret;
}

REDEF_CMD(emacs_delete)
{
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *m;

	if (!ci->mark)
		return Enoarg;

	m = mark_dup(ci->mark);

	ret = call(mv->type, ci->focus, mv->direction * RPT_NUM(ci), m);

	if (!ret) {
		mark_free(m);
		return 0;
	}
	ret = call("Replace", ci->focus, 1, m, NULL, N2(ci) == N2_undo_delete);
	mark_free(m);
	call("Mode:set-num2", ci->focus, N2_undo_delete);

	return ret;
}

REDEF_CMD(emacs_kill)
{
	/* Like delete, but copy to copy-buffer */
	struct move_command *mv = container_of(ci->comm, struct move_command, cmd);
	int ret = 0;
	struct mark *m;
	char *str;

	if (!ci->mark)
		return Enoarg;

	m = mark_dup(ci->mark);

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
	str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, m);
	if (str && *str)
		call("copy:save", ci->focus, N2(ci) == N2_undo_delete, NULL, str);
	ret = call("Replace", ci->focus, 1, m, NULL, N2(ci) == N2_undo_delete);
	mark_free(m);
	call("Mode:set-num2", ci->focus, N2_undo_delete);

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
		return Enoarg;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark);
	}

	while (cnt > 0) {
		struct mark *m = mark_dup(ci->mark);

		ret = call(mv->type+1, ci->focus, dir, ci->mark);
		if (ret <= 0 || mark_same(ci->mark, m))
			/* Hit end of file */
			cnt = 1;
		else {
			char *str = call_ret(str, "doc:get-str", ci->focus,
					     0, ci->mark, NULL,
					     0, m);
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
				ret = call("Replace", ci->focus, 1, m, str,
					   N2(ci) == N2_undo_change);
				if (dir < 0)
					call(mv->type+1, ci->focus, dir, ci->mark);
			}
			free(str);
			call("Mode:set-num2", ci->focus, N2_undo_change);
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
		return Enoarg;

	if (cnt == 0)
		return 1;
	if (cnt > 0) {
		dir = 1;
	} else {
		dir = -1;
		cnt = -cnt;
		start = mark_dup(ci->mark);
	}

	while (cnt > 0) {
		struct mark *as, *ae, *bs, *be;
		char *astr, *bstr;

		ret = call(mv->type, ci->focus, -dir, ci->mark);
		if (ret <= 0)
			break;
		as = mark_dup(ci->mark);
		ret = call(mv->type, ci->focus, dir, ci->mark);
		if (ret <= 0 || mark_same(ci->mark, as)) {
			mark_free(as);
			break;
		}
		ae = mark_dup(ci->mark);
		call(mv->type, ci->focus, dir, ci->mark);
		be = mark_dup(ci->mark);
		call(mv->type, ci->focus, -dir, ci->mark);
		bs = mark_dup(ci->mark);
		astr = call_ret(str, "doc:get-str", ci->focus,
				0, as, NULL,
				0, ae);
		bstr = call_ret(str, "doc:get-str", ci->focus,
				0, bs, NULL,
				0, be);
		mark_to_mark(ci->mark, ae);
		call("Replace", ci->focus, 1, as, bstr);
		mark_to_mark(ci->mark, be);
		call("Replace", ci->focus, 1, bs, astr, 1);
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

DEF_CMD(emacs_move_view_other)
{
	/* If there is an 'other' pane', Send "Next" there */
	struct pane *p;

	/* '512' means 'fail if no other pane' */
	p = call_ret(pane, "OtherPane", ci->focus, 512);
	if (!p)
		return 1;
	call("Mode:set-num", p, ci->num);
	call("Keystroke", p, 0, NULL, "emacs-move-large-other");
	return 1;
}

DEF_CMD(emacs_recenter)
{
	int step = 0;
	if (ci->num == NO_NUMERIC && N2(ci) == N2_recentre) {
		/* Repeated command - go to top, or bottom, or middle in order */
		switch (N2a(ci)) {
		default:
		case 0: /* was center, go to top */
			call("Move-View-Line", ci->focus, 1, ci->mark);
			step = 1;
			break;
		case 1: /* was top, go to bottom */
			call("Move-View-Line", ci->focus, -1, ci->mark);
			step = 2;
			break;
		case 2: /* was bottom, go to middle */
			call("Move-View-Line", ci->focus, 0, ci->mark);
			step = 0;
			break;
		}
	} else if (ci->num == -NO_NUMERIC) {
		/* Move point to bottom */
		call("Move-View-Line", ci->focus, -1, ci->mark);
		step = 2;
	} else if (ci->num != NO_NUMERIC) {
		/* Move point to display line N */
		call("Move-View-Line", ci->focus, ci->num, ci->mark);
	} else {
		/* Move point to middle and refresh */
		call("Move-View-Line", ci->focus, 0, ci->mark);
		call("Display:refresh", ci->focus);
	}
	call("Mode:set-num2", ci->focus, N2_recentre | (step << 16));
	return 1;
}

REDEF_CMD(emacs_simple);
REDEF_CMD(emacs_simple_neg);
REDEF_CMD(emacs_simple_num);
REDEF_CMD(emacs_simple_str);
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
	{CMD(emacs_simple), "Window:bury", "M-Chr-B"},
	{CMD(emacs_simple), "Display:new", "emCX5-Chr-2"},
	{CMD(emacs_simple), "Display:close", "emCX5-Chr-0"},
	{CMD(emacs_simple), "lib-server:done", "emCX-Chr-#"},
	{CMD(emacs_simple), "Abort", "C-Chr-G"},
	{CMD(emacs_simple), "NOP", "M-Chr-G"},
	{CMD(emacs_simple), "NOP", "emCX-C-Chr-G"},
	{CMD(emacs_simple), "NOP", "emCX4-C-Chr-G"},
	{CMD(emacs_simple), "doc:save-file", "emCX-C-Chr-S"},
	/* one day, this will be "find definition", now it is same as "find any" */
	{CMD(emacs_simple_num), "interactive-cmd-git-grep", "emCX-M-Chr-."},
	{CMD(emacs_simple_str), "interactive-cmd-git-grep", "M-Chr-."},
};

REDEF_CMD(emacs_simple)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return Enoarg;

	return call(sc->type, ci->focus, ci->num, ci->mark, NULL, ci->num2);
}

REDEF_CMD(emacs_simple_neg)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return Enoarg;

	return call(sc->type, ci->focus, -RPT_NUM(ci), ci->mark, NULL, ci->num2);
}

REDEF_CMD(emacs_simple_num)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);

	if (!ci->mark)
		return Enoarg;

	return call(sc->type, ci->focus, RPT_NUM(ci), ci->mark, NULL, ci->num2);
}

REDEF_CMD(emacs_simple_str)
{
	struct simple_command *sc = container_of(ci->comm, struct simple_command, cmd);
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	char *str = NULL;

	if (!ci->mark)
		return Enoarg;
	if (mk && attr_find_int(mk->attrs, "emacs:active") >= 1) {
		str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);
		/* Disable mark */
		attr_set_int(&mk->attrs, "emacs:active", 0);
		/* Clear current highlight */
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
	}

	return call(sc->type, ci->focus, RPT_NUM(ci), ci->mark, str, ci->num2);
}

DEF_CMD(cnt_disp)
{
	struct call_return *cr = container_of(ci->comm, struct call_return, c);

	cr->i += 1;
	return 1;
}

DEF_CMD(emacs_exit)
{
	struct call_return cr;

	cr.c = cnt_disp;
	cr.i = 0;
	if (ci->num == NO_NUMERIC) {
		struct pane *p;

		/* If this is not only display, then refuse to exit */
		call_comm("editor:notify:all-displays", ci->focus, &cr.c);
		if (cr.i > 1) {
			call("Message", ci->focus, 0, NULL,
			     "Cannot exit when there are multiple windows.");
			return 1;
		}

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM");
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
		return Enoarg;

	/* Key is "Chr-X" - skip 4 bytes to get X */
	str = ci->key + 4;
	ret = call("Replace", ci->focus, 1, ci->mark, str,
		   N2(ci) == N2_undo_insert);
	call("Mode:set-num2", ci->focus, N2_undo_insert);

	return ret;
}

DEF_CMD(emacs_quote_insert)
{
	int ret;
	char buf[2] = ".";
	char *str = buf;

	if (!ci->mark)
		return Enoarg;

	if (strncmp(ci->key, "emQ-Chr-", 8) == 0)
		str = ci->key + 8;
	else if (strncmp(ci->key, "emQ-C-Chr-", 10) == 0)
		buf[0] = ci->key[10] & 0x1f;
	else
		str = "??";
	ret = call("Replace", ci->focus, 1, ci->mark, str,
		   N2(ci) == N2_undo_insert);
	call("Mode:set-num2", ci->focus, N2_undo_insert);

	return ret;
}

static struct {
	char *key;
	char *insert;
} other_inserts[] = {
	{"Tab", "\t"},
	{"LF", "\n"},
	{"Enter", "\n"},
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
		return Enoarg;

	for (i = 0; other_inserts[i].key; i++)
		if (strcmp(safe_cast other_inserts[i].key, ci->key) == 0)
			break;
	ins = other_inserts[i].insert;
	if (ins == NULL)
		return 0;

	if (!*ins) {
		ins++;
		m = mark_dup(ci->mark);
		/* Move m before ci->mark, so it doesn't move when we insert */
		mark_make_first(m);
	}

	ret = call("Replace", ci->focus, 1, m, ins,
		   N2(ci) == N2_undo_insert, ci->mark);
	if (m) {
		mark_to_mark(ci->mark, m);
		mark_free(m);
	}
	/* A newline starts a new undo */
	call("Mode:set-num2", ci->focus, (*ins == '\n') ? 0 : N2_undo_insert);
	return ret;
}

DEF_CMD(emacs_interactive_insert)
{
	/* If some pane want to insert text just like it was typed,
	 * it calls this, and we set up for proper undo
	 */
	int ret;

	if (!ci->str)
		return Enoarg;
	ret = call("Replace", ci->focus, 1, ci->mark, ci->str,
		   N2(ci) == N2_undo_insert);
	call("Mode:set-num2", ci->focus,
	     strchr(ci->str, '\n') ? 0 : N2_undo_insert);
	return ret;
}

DEF_CMD(emacs_interactive_delete)
{
	/* If some pane want to delete text just like backspace was typed,
	 * it calls this, and we set up for proper undo
	 */
	int ret;

	if (!ci->str)
		return Enoarg;
	ret = call("Replace", ci->focus, 1, ci->mark, "",
		   N2(ci) == N2_undo_insert, ci->mark2);
	call("Mode:set-num2", ci->focus,
	     strchr(ci->str, '\n') ? 0 : N2_undo_delete);
	return ret;
}

DEF_CMD(emacs_undo)
{
	int ret;

	ret = call("doc:reundo", ci->focus, N2(ci) == N2_undo);
	call("Mode:set-num2", ci->focus, N2_undo);
	if (ret == Efalse)
		call("Message", ci->focus, 0, NULL,
		     "No further undo information");

	return 1;
}

REDEF_CMD(emacs_file_complete);
REDEF_CMD(emacs_doc_complete);

DEF_CMD(find_complete)
{
	char *type = ci->home->data;

	if (strcmp(type, "file") == 0)
		return emacs_file_complete_func(ci);
	if (strcmp(type, "shellcmd") == 0)
		return emacs_file_complete_func(ci);
	if (strcmp(type, "doc") == 0)
		return emacs_doc_complete_func(ci);
	return 0;
}

DEF_CMD(find_done)
{
	int ret;
	char *type = ci->home->data;
	char *str = call_ret(strsave, "doc:get-str", ci->focus);
	struct stat stb;

	if (!str || !str[0])
		/* close with no string */
		ret = call("popup:close", ci->focus);
	if (strcmp(type, "doc") == 0 &&
	    call_ret(pane, "docs:byname", ci->focus, 0, NULL, str) == NULL) {
		call("Message:modal", ci->focus, 0, NULL, "Document not found");
		return 1;
	}
	if (strcmp(type, "file") == 0 &&
	    strcmp(ci->key, "Enter") == 0 &&
	    stat(file_normalize(ci->focus, str, pane_attr_get(ci->focus,
							      "initial_path")),
		 &stb) != 0) {
		call("Message:modal", ci->focus, 0, NULL,
		     "File not found - use Alt-Enter to create");
		return 1;
	}
	ret = call("popup:close", ci->focus, 0, NULL, str);
	return ret;
}

struct find_helper {
	char *name;
	int want_prev;
	struct pane *ret;
	struct command c;
};

DEF_CMD(find_helper)
{
	struct find_helper *h = container_of(ci->comm, struct find_helper, c);
	struct pane *p = ci->focus;
	char *name;

	if (!p)
		return 1;
	if (!h->name) {
		if (h->want_prev) {
			/* want the pane before nothing, so the last.
			 * So return this one and keep looking.
			 */
			h->ret = p;
			return 0;
		} else {
			/* Want the pane that is after nothing, so
			 * the first.  This one. All done.
			 */
			h->ret = p;
			return 1;
		}
	}
	name = pane_attr_get(ci->focus, "doc-name");
	if (!name)
		return 0;
	if (strcmp(name, h->name) == 0) {
		if (h->want_prev) {
			/* Want the previous one, which is
			 * already in ->ret
			 */
			return 1;
		} else {
			/* Want the next one, so clear name
			 * and keep going.
			 */
			h->name = NULL;
			return 0;
		}
	} else {
		if (h->want_prev) {
			/* This might be what I want - keep it in case */
			h->ret = p;
			return 0;
		} else {
			/* Don't want this - just keep going */
			return 0;
		}
	}
}

DEF_CMD(find_prevnext)
{
	/* Find the previous document lru or, which is "next" as we
	 * walk the list in mru order.
	 * When we find it, insert the name into ci->focus document
	 */
	char *type = ci->home->data;
	struct find_helper h;

	if (strcmp(type, "doc") != 0)
		return 0;
	h.name = attr_find(ci->home->attrs, "find-doc");
	h.ret = NULL;
	h.c = find_helper;
	h.want_prev = strcmp(ci->key, "M-Chr-n") == 0;

	call_comm("docs:byeach", ci->focus, &h.c);
	if (h.ret) {
		char *name = pane_attr_get(h.ret, "doc-name");
		struct mark *m, *m2;

		attr_set_str(&ci->home->attrs, "find-doc", name);
		m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
		m2 = m ? mark_dup(m) : NULL;
		call("Move-file", ci->focus, -1, m);
		call("Move-file", ci->focus, 1, m2);
		call("Replace", ci->focus, 1, m, name, 0, m2);
		mark_free(m);
		mark_free(m2);
	}
	return 1;

}

static struct
map *fh_map;
static void findmap_init(void)
{
	fh_map = key_alloc();
	key_add(fh_map, "Tab", &find_complete);
	key_add(fh_map, "Enter", &find_done);
	key_add(fh_map, "M-Enter", &find_done);
	key_add(fh_map, "M-Chr-p", &find_prevnext);
	key_add(fh_map, "M-Chr-n", &find_prevnext);
}

static char * safe file_normalize(struct pane *p safe, char *path, char *initial_path)
{
	int len = strlen(initial_path?:"");
	char *dir;

	if (!path)
		return ".";
	if (initial_path && strncmp(path, initial_path, len) == 0) {
		if (path[len] == '/' || (path[len] == '~' &&
					 (path[len+1] == '/' || path[len+1] == 0)))
			path = path + len;
	}
	if (path[0] == '/')
		return path;
	if (path[0] == '~' && (path[1] == '/' || path[1] == 0)) {
		char *home = getenv("HOME");
		if (!home)
			home = "/";
		if (home[strlen(home) - 1] == '/' && path[1] == '/')
			path += 1;
		return strconcat(p, home, path+1);
	}
	dir = pane_attr_get(p, "dirname");
	if (!dir)
		dir = "/";
	if (dir[strlen(dir)-1] == '/')
		return strconcat(p, dir, path);
	return strconcat(p, dir, "/", path);
}

DEF_LOOKUP_CMD(find_handle, fh_map);

DEF_CMD(emacs_findfile)
{
	int fd;
	struct pane *p, *par;
	char *path;
	char buf[PATH_MAX];
	char *e;

	path = pane_attr_get(ci->focus, "dirname");
	if (path) {
		strcpy(buf, path);
		path = buf;
	}
	if (!path) {
		strcpy(buf, "/");
		path = buf;
	}
	e = buf + strlen(buf);
	if (e < buf + sizeof(buf)-1 && e > buf && e[-1] != '/') {
		*e++ = '/';
		*e++ = '\0';
	}

	if (strncmp(ci->key, "File Found", 10) != 0) {
		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, path);
		if (!p)
			return 0;

		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prompt",
				     "Find File Other Window");
			attr_set_str(&p->attrs, "done-key",
				     "File Found Other Window");
		} else {
			attr_set_str(&p->attrs, "prompt", "Find File");
			attr_set_str(&p->attrs, "done-key", "File Found");
		}
		call("doc:set-name", p, 0, NULL, "Find File");

		p = pane_register(p, 0, &find_handle.c, "file");
		attr_set_str(&p->attrs, "initial_path", path);
		call("attach-history", p, 0, NULL, "*File History*",
		     0, NULL, "popup:close");
		return 1;
	}

	path = file_normalize(ci->focus, ci->str, path);

	if (!path)
		return Efail;

	fd = open(path, O_RDONLY);
	if (fd >= 0) {
		p = call_ret(pane, "doc:open", ci->focus, fd, NULL, path);
		close(fd);
	} else
		/* '4' says 'allow create' */
		p = call_ret(pane, "doc:open", ci->focus, -1, NULL, path, 4);
	if (!p) {
		char *m = NULL;
		asprintf(&m, "Failed to open file: %s", path);
		call("Message", ci->focus, 0, NULL, m);
		free(m);
		return Efail;
	}
	if (strcmp(ci->key, "File Found Other Window") == 0) {
		par = home_call_ret(pane, ci->focus, "DocPane", p);
		if (!par)
			par = call_ret(pane, "OtherPane", ci->focus);
	} else
		par = call_ret(pane, "ThisPane", ci->focus);

	if (!par) {
		pane_close(p);
		return Efail;
	}

	p = home_call_ret(pane, p, "doc:attach-view", par, 1);
	if (p)
		pane_focus(p);
	return p ? 1 : Efail;
}

REDEF_CMD(emacs_file_complete)
{
	/* Extract a directory name and a basename from the document.
	 * Find a document for the directory and attach as a completing
	 * popup menu
	 */
	char *str;
	char *d, *b;
	int fd;
	struct pane *pop, *docp, *p;
	struct call_return cr;
	char *type = ci->home->data;
	char *initial = attr_find(ci->home->attrs, "initial_path");
	int wholebuf = strcmp(type, "file") == 0;

	if (!ci->mark)
		return Enoarg;

	str = call_ret(strsave, "doc:get-str", ci->focus);
	if (!str)
		return Einval;
	if (wholebuf) {
		d = str;
	} else {
		initial = "";
		d = str + strlen(str);
		while (d > str && d[-1] != ' ')
			d -= 1;
	}
	d = file_normalize(ci->focus, d, initial);
	b = strrchr(d, '/');
	if (b) {
		b += 1;
		d = strnsave(ci->focus, d, b-d);
	} else {
		b = d;
		d = strsave(ci->focus, ".");
	}
	fd = open(d, O_DIRECTORY|O_RDONLY);
	if (fd < 0) {
		return Efail;
	}
	/* 32 means quiet */
	docp = call_ret(pane, "doc:open", ci->focus, fd, NULL, d, 32);
	close(fd);
	if (!docp)
		return Efail;
	pop = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		return Efail;
	p = home_call_ret(pane, docp, "doc:attach-view", pop, -1, NULL, "complete");
	if (!p)
		return Efail;
	//call("doc:notify:doc:revisit", p, -1);

	cr = call_ret(all, "Complete:prefix", p, 1, NULL, b);
	if (cr.s && (strlen(cr.s) <= strlen(b) && cr.ret-1 > 1)) {
		/* We need the dropdown - delete prefix and drop-down will
		 * insert result.
		 */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("Move-Char", ci->focus, -strlen(b), start);
		call("Replace", ci->focus, 1, start);
		mark_free(start);

		return 1;
	}
	if (cr.s) {
		/* Replace 'b' with the result. */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("Move-Char", ci->focus, -strlen(b), start);
		call("Replace", ci->focus, 1, start, cr.s);
		mark_free(start);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_finddoc)
{
	struct pane *p, *par;

	if (strncmp(ci->key, "Doc Found", 9) != 0) {
		struct pane *dflt;
		char *defname = NULL;

		dflt = call_ret(pane, "docs:choose", ci->focus);
		if (dflt)
			defname = pane_attr_get(dflt, "doc-name");

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
		if (!p)
			return 0;

		if (defname)
			attr_set_str(&p->attrs, "default", defname);
		if (strncmp(ci->key, "emCX4-", 6) == 0) {
			attr_set_str(&p->attrs, "prompt",
				     "Find Document Other Window");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found Other Window");
		} else {
			attr_set_str(&p->attrs, "prompt", "Find Document");
			attr_set_str(&p->attrs, "done-key", "Doc Found");
		}
		call("doc:set-name", p, 0, NULL, "Find Document", -1);

		pane_register(p, 0, &find_handle.c, "doc");
		return 1;
	}

	p = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (!p)
		return Efail;

	if (strcmp(ci->key, "Doc Found Other Window") == 0) {
		par = home_call_ret(pane, ci->focus, "DocPane", p);
		if (!par)
			par = call_ret(pane, "OtherPane", ci->focus);
	} else
		par = call_ret(pane, "ThisPane", ci->focus);
	if (!p || !par)
		return Efail;

	p = home_call_ret(pane, p, "doc:attach-view", par, 1);
	if (p)
		pane_focus(p);
	return p ? 1 : Efail;
}

REDEF_CMD(emacs_doc_complete)
{
	/* Extract a document name from the document.
	 * Attach the 'docs' document as a completing popup menu
	 */
	char *str;
	struct pane *pop, *p = NULL;
	struct call_return cr;

	if (!ci->mark)
		return Enoarg;

	str = call_ret(strsave, "doc:get-str", ci->focus);
	if (!str)
		return Einval;
	pop = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM1r");
	if (!pop)
		return Efail;
	p = call_ret(pane, "docs:byname", ci->focus);
	if (p)
		p = home_call_ret(pane, p, "doc:attach-view", pop,
				  0, NULL, "complete");
	if (!p)
		return Efail;
	cr = call_ret(all, "Complete:prefix", p, 1, NULL, str);
	if (cr.s && (strlen(cr.s) <= strlen(str) && cr.ret - 1 > 1)) {
		/* We need the dropdown */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("doc:set-ref", ci->focus, 1, start);

		call("Replace", ci->focus, 1, start);
		mark_free(start);
		return 1;
	}
	if (cr.s) {
		/* Replace the prefix with the new value */
		struct mark *start;

		start = mark_dup(ci->mark);
		call("doc:set-ref", ci->focus, 1, start);

		call("Replace", ci->focus, 1, start, cr.s);
		mark_free(start);
	}
	/* Now need to close the popup */
	pane_close(pop);
	return 1;
}

DEF_CMD(emacs_viewdocs)
{
	struct pane *p, *par;
	struct pane *docs;

	docs = call_ret(pane, "docs:byname", ci->focus);
	if (!docs)
		return Efail;
	par = call_ret(pane, "ThisPane", ci->focus);
	if (!par)
		return Efail;

	p = home_call_ret(pane, docs, "doc:attach-view", par, 1);
	return !!p;
}

DEF_CMD(emacs_shell)
{
	char *name = "*Shell Command Output*";
	struct pane *p, *doc, *par;
	char *path;

	if (strcmp(ci->key, "Shell Command") != 0) {
		char *dirname;

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
		if (!p)
			return 0;
		dirname = call_ret(strsave, "get-attr", ci->focus, 0, NULL, "dirname");
		attr_set_str(&p->attrs, "dirname", dirname ?: ".");
		attr_set_str(&p->attrs, "prompt", "Shell command");
		attr_set_str(&p->attrs, "done-key", "Shell Command");
		call("doc:set-name", p, 0, NULL, "Shell Command", -1);
		p = call_ret(pane, "attach-history", p, 0, NULL, "*Shell History*",
			     0, NULL, "popup:close");
		pane_register(p, 0, &find_handle.c, "shellcmd");
		return 1;
	}
	path = pane_attr_get(ci->focus, "dirname");
	/* Find or create "*Shell Command Output*" */
	doc = call_ret(pane, "docs:byname", ci->focus, 0, NULL, name);
	if (!doc)
		doc = call_ret(pane, "doc:from-text", ci->focus, 0, NULL, name, 0, NULL, "");
	if (!doc)
		return Efail;
	attr_set_str(&doc->attrs, "dirname", path);
	par = home_call_ret(pane, ci->focus, "DocPane", doc);
	if (!par)
		par = call_ret(pane, "OtherPane", ci->focus);
	if (!par)
		return Efail;
	/* shellcmd is attached directly to the document, not in the view
	 * stack.  It is go-between for document and external command.
	 * We don't need a doc attachment as no point is needed - we
	 * always insert at the end.
	 */
	call_ret(pane, "attach-shellcmd", doc, 0, NULL, ci->str, 0, NULL, path);

	if (strstr(ci->str, "diff") || strstr(ci->str, "git show"))
		attr_set_str(&doc->attrs, "view-default", "diff");
	else
		attr_set_str(&doc->attrs, "view-default", "viewer");
	home_call(doc, "doc:attach-view", par, 1, NULL);

	return 1;
}

DEF_CMD(emacs_num)
{
	int rpt = ci->num;
	char *last = ci->key + strlen(ci->key)-1;
	int neg = 0;

	if (rpt < 0) {
		neg = 1;
		rpt = -rpt;
	}
	if (rpt == NO_NUMERIC)
		rpt = 0;

	rpt = rpt * 10 + *last - '0';

	call("Mode:set-num", ci->focus, neg ? -rpt : rpt);
	call("Mode:set-num2", ci->focus, ci->num2);
	return 1;
}

DEF_CMD(emacs_neg)
{
	call("Mode:set-num", ci->focus, - ci->num);
	call("Mode:set-num2", ci->focus, ci->num2);
	return 1;
}

DEF_CMD(emacs_prefix)
{
	/* as a generic arg (ctrl-U) which is positive and
	 * as as a repeat-count of 4, but is different to 4.
	 * I should probably allow digits to alter the number.
	 */
	call("Mode:set-num", ci->focus, 4);
	return 1;
}

DEF_CMD(emacs_kill_doc)
{
	return call("doc:destroy", ci->focus);
}

DEF_CMD(emacs_revisit)
{
	return call("doc:load-file", ci->focus, 2, NULL, NULL, -1);
}

DEF_CMD(emacs_save_all)
{
	int ret = call("docs:save-all", ci->focus, 0, NULL, NULL, 1);

	if (ret == 1) {
		call("Message", ci->focus, 0, NULL, "No files need to be saved.");
		return 1;
	}
	if (ci->num == NO_NUMERIC) {
		struct pane *p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "DM");
		if (p)
			return call("docs:show-modified", p);
	}
	return call("docs:save-all", ci->focus);
}

static void do_searches(struct pane *p safe,
			struct pane *owner, int view, char *patn,
			int ci,
			struct mark *m, struct mark *end)
{
	int ret;
	if (!m)
		return;
	m = mark_dup(m);
	while ((ret = call("text-search", p, ci, m, patn, 0, end)) >= 1) {
		struct mark *m2, *m3;
		int len = ret - 1;
		m2 = vmark_new(p, view, owner);
		if (!m2)
			break;
		mark_to_mark(m2, m);
		while (ret > 1 && mark_prev_pane(p, m2) != WEOF)
			ret -= 1;
		m3 = vmark_matching(m2);
		if (m3) {
			mark_free(m2);
			m2 = m3;
		}
		if (attr_find(m2->attrs, "render:search") == NULL) {
			attr_set_int(&m2->attrs, "render:search2", len);
			m2 = vmark_new(p, view, owner);
			if (m2) {
				mark_to_mark(m2, m);
				attr_set_int(&m2->attrs, "render:search2-end", 0);
			}
		}
		if (len == 0)
			/* Need to move forward, or we'll just match here again*/
			mark_next_pane(p, m);
	}
	mark_free(m);
}

struct highlight_info {
	int view;
	char *patn;
	int ci;
	struct mark *start, *end;
	struct pane *popup safe;
};

DEF_CMD(emacs_search_highlight)
{
	/* from 'mark' for 'num' chars to 'mark2' there is a match for 'str',
	 * or else there are no matches (num==0).
	 * Here we remove any existing highlighting and highlight
	 * just the match.  A subsequent call to emacs_search_reposition
	 * will highlight other near-by matches.
	 */
	struct mark *m, *start;
	struct highlight_info *hi = ci->home->data;

	if (hi->view < 0)
		return 0;

	while ((start = vmark_first(ci->focus, hi->view, ci->home)) != NULL)
		mark_free(start);

	free(hi->patn);
	if (ci->str)
		hi->patn = strdup(ci->str);
	else
		hi->patn = NULL;
	hi->ci = ci->num2;

	if (ci->mark && ci->num >= 0 && ci->str) {
		m = vmark_new(ci->focus, hi->view, ci->home);
		if (!m)
			return Efail;
		mark_to_mark(m, ci->mark);
		attr_set_int(&m->attrs, "render:search", ci->num);
		call("Move-View-Pos", ci->focus, 0, m);
		if (ci->mark2 &&
		    (m = vmark_new(ci->focus, hi->view, ci->home)) != NULL) {
			mark_to_mark(m, ci->mark2);
			attr_set_int(&m->attrs, "render:search-end", 0);
		}
	}
	call("view:changed", ci->focus);
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
		 * the right direction.  That will mean it has moved
		 * off the display.
		 */
		m = mark_at_point(ci->focus, NULL, MARK_UNGROUPED);
		if (m) {
			struct mark *m2 = mark_dup(m);
			call("Move-CursorXY", ci->focus, 1, m, NULL,
			     0, NULL, NULL,
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

DEF_CMD(emacs_search_reposition_delayed)
{
	struct highlight_info *hi = ci->home->data;
	struct mark *start = hi->start;
	struct mark *end = hi->end;
	struct mark *vstart, *vend;
	char *patn = hi->patn;
	int damage = 0;

	if (!start || !end)
		return Efalse;

	vstart = vmark_first(ci->focus, hi->view, ci->home);
	vend = vmark_last(ci->focus, hi->view, ci->home);
	if (vstart == NULL || start->seq < vstart->seq) {
		/* search from 'start' to first match or 'end' */
		do_searches(ci->focus, ci->home, hi->view, patn, hi->ci, start, vstart ?: end);
		if (vend)
			do_searches(ci->focus, ci->home, hi->view, patn, hi->ci,
				    vend, end);
	} else if (vend && end->seq > vend->seq) {
		/* search from last match to end */
		do_searches(ci->focus, ci->home, hi->view, patn, hi->ci, vend, end);
	}
	if (vstart != vmark_first(ci->focus, hi->view, ci->home) ||
	    vend != vmark_last(ci->focus, hi->view, ci->home))
		damage = 1;
	if (damage) {
		pane_damaged(ci->focus, DAMAGED_CONTENT);
		pane_damaged(ci->focus, DAMAGED_VIEW);
	}
	mark_free(hi->start);
	mark_free(hi->end);
	hi->start = hi->end = NULL;
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
	char *patn = hi->patn;
	int damage = 0;
	struct mark *m;

	if (hi->view < 0 || patn == NULL || !start || !end)
		return 0;

	while ((m = vmark_first(ci->focus, hi->view, ci->home)) != NULL &&
	       mark_ordered_not_same(m, start)) {
		mark_free(m);
		damage = 1;
	}
	while ((m = vmark_last(ci->focus, hi->view, ci->home)) != NULL &&
	       mark_ordered_not_same(end, m)) {
		mark_free(m);
		damage = 1;
	}
	mark_free(hi->start);
	mark_free(hi->end);
	hi->start = mark_dup(start);
	hi->end = mark_dup(end);

	if (damage) {
		pane_damaged(ci->focus, DAMAGED_CONTENT);
		pane_damaged(ci->focus, DAMAGED_VIEW);
	}
	call_comm("event:timer", ci->focus, &emacs_search_reposition_delayed,
		  500);
	return 1;
}

DEF_LOOKUP_CMD(highlight_handle, hl_map);

DEF_CMD(emacs_start_search)
{
	struct pane *p, *hp;
	struct highlight_info *hi = calloc(1, sizeof(*hi));

	hp = pane_register(ci->focus, 0, &highlight_handle.c, hi);
	hi->view = home_call(ci->focus, "doc:add-view", hp) - 1;

	p = call_ret(pane, "PopupTile", hp, 0, NULL, "TR2", 0, NULL, "");

	if (!p)
		return 0;
	hi->popup = p;

	attr_set_str(&p->attrs, "prompt", "Search");
	attr_set_str(&p->attrs, "done-key", "Search String");
	call("doc:set-name", p, 0, NULL, "Search", -1);
	call_ret(pane, "attach-emacs-search", p, ci->key[6] == 'R');

	return 1;
}

DEF_CMD(emacs_highlight_close)
{
	/* ci->focus is being closed */
	struct highlight_info *hi = ci->home->data;

	free(hi->patn);
	if (hi->view >= 0) {
		struct mark *m;

		while ((m = vmark_first(ci->focus, hi->view, ci->home)) != NULL)
			mark_free(m);
		call("doc:del-view", ci->home, hi->view);
	}
	mark_free(hi->start);
	mark_free(hi->end);
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

DEF_CMD(emacs_highlight_abort)
{
	pane_close(ci->home);
	return 0;
}

DEF_CMD(emacs_highlight_clip)
{
	struct highlight_info *hi = ci->home->data;

	marks_clip(ci->home, ci->mark, ci->mark2, hi->view, ci->home);
	return 0;
}

DEF_CMD(emacs_command)
{
	struct pane *p;

	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
	if (!p)
		return 0;
	attr_set_str(&p->attrs, "prompt", "Cmd");
	attr_set_str(&p->attrs, "done-key", "emacs:command");
	call("doc:set-name", p, 0, NULL, "M-x command", -1);
	p = call_ret(pane, "attach-history", p, 0, NULL, "*Command History*",
		     0, NULL, "popup:close");
	pane_register(p, 0, &find_handle.c, "cmd");
	return 1;
}

DEF_CMD(emacs_do_command)
{
	char cmd[30];
	int ret;

	snprintf(cmd, sizeof(cmd), "interactive-cmd-%s", ci->str);
	ret = call(cmd, ci->focus, 0, ci->mark);
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
	char *v = NULL;

	asprintf(&v, "%s ; emacs-mode - v" VERSION " - " VERS_DATE, edlib_version);
	if (v)
		call("Message", ci->focus, 0, NULL, v);
	free(v);
	return 1;
}

DEF_CMD(emacs_mark)
{
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	struct mark *m = call_ret(mark2, "doc:point", ci->focus);

	if (m && attr_find_int(m->attrs, "emacs:active") >= 1)
		/* Clear current highlight */
		call("view:changed", ci->focus, 0, p, NULL, 0, m);
	call("Move-to", ci->focus, 1);
	m = call_ret(mark2, "doc:point", ci->focus);
	if (m)
		attr_set_int(&m->attrs, "emacs:active", 1);
	return 1;
}

DEF_CMD(emacs_abort)
{
	/* On abort, forget mark */
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	struct mark *m = call_ret(mark2, "doc:point", ci->focus);

	if (m && attr_find_int(m->attrs, "emacs:active") >= 1) {
		/* Clear current highlight */
		call("view:changed", ci->focus, 0, p, NULL, 0, m);
		attr_set_int(&m->attrs, "emacs:active", 0);
	}
	return 0;
}

DEF_CMD(emacs_swap_mark)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *m;

	if (!mk)
		return 1;
	m = mark_dup(mk);
	call("Move-to", ci->focus, 1); /* Move mark to point */
	call("Move-to", ci->focus, 0, m); /* Move point to old mark */
	attr_set_int(&mk->attrs, "emacs:active", 1);
	mark_free(m);
	return 1;
}

DEF_CMD(emacs_wipe)
{
	/* Delete text from point to mark */
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	char *str;
	int ret;

	if (!mk)
		return 1;
	str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);
	if (str && *str)
		call("copy:save", ci->focus, 0, NULL, str);
	ret = call("Replace", ci->focus, 1, mk);
	/* Clear mark */
	attr_set_int(&mk->attrs, "emacs:active", 0);

	return ret;
}

DEF_CMD(emacs_copy)
{
	/* copy text from point to mark */
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	char *str;

	if (!mk)
		return 1;
	str = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);
	if (str && *str)
		call("copy:save", ci->focus, 0, NULL, str);
	/* Clear current highlight */
	if (attr_find_int(mk->attrs, "emacs:active") >= 1) {
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
		/* Disable mark */
		attr_set_int(&mk->attrs, "emacs:active", 0);
	}
	return 1;
}

DEF_CMD(emacs_yank)
{
	int n = RPT_NUM(ci);
	char *str = call_ret(strsave, "copy:get", ci->focus, n - 1);
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *m = NULL;

	if (!str || !*str)
		return 1;
	/* If mark exists and is active, replace marked regions */
	if (mk && attr_find_int(mk->attrs, "emacs:active") > 0) {
		char *str2 = call_ret(strsave, "doc:get-str", ci->focus, 0, NULL, NULL, 0, mk);
		if (str2 && *str2)
			call("copy:save", ci->focus, 0, NULL, str2);
		m = mark_dup(mk);
	}

	call("Move-to", ci->focus, 1);
	call("Replace", ci->focus, 1, m, str);
	mk = call_ret(mark2, "doc:point", ci->focus);
	if (mk)
		attr_set_int(&mk->attrs, "emacs:active", 0);
	call("Mode:set-num2", ci->focus, N2_yank);
	return 1;
}

DEF_CMD(emacs_yank_pop)
{
	struct mark *mk, *m;
	char *str;
	int num = N2a(ci);

	if (N2(ci) != N2_yank)
		return Einval;
	mk = call_ret(mark2, "doc:point", ci->focus);
	if (!mk)
		return 1;
	num += 1;
	str = call_ret(strsave, "copy:get", ci->focus, num);
	if (!str) {
		num = 0;
		str = call_ret(strsave, "copy:get", ci->focus, num);
	}
	if (!str)
		return Efail;
	m = mark_dup(mk);
	mark_make_first(m);
	call("Replace", ci->focus, 1, mk, str);
	call("Move-to", ci->focus, 1, m);
	mark_free(m);
	call("Mode:set-num2", ci->focus, (num << 16) | N2_yank);
	return 1;
}

DEF_CMD(emacs_attrs)
{
	struct call_return cr;
	static char selection[] = "bg:red+80,vis-nl"; // pink

	if (!ci->str)
		return 0;

	cr = call_ret(all, "doc:point", ci->focus);
	if (cr.ret <= 0 || !cr.m || !cr.m2 || !ci->mark)
		return 1;
	if (attr_find_int(cr.m2->attrs, "emacs:active") <= 0)
		return 1;
	if (mark_same(cr.m, cr.m2))
		return 1;
	if (strcmp(ci->str, "render:interactive-mark") == 0) {
		if (ci->mark == cr.m2 && cr.m2->seq < cr.m->seq)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 2000000,
					 ci->mark, selection, 2);
		if (ci->mark == cr.m2)
			return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
					 ci->mark, selection, 2);
	}
	if (strcmp(ci->str, "render:interactive-point") == 0) {
		if (cr.m == ci->mark && cr.m->seq < cr.m2->seq)
			return comm_call(ci->comm2, "attr:cb", ci->focus, 2000000,
					 ci->mark, selection, 2);
		if (cr.m == ci->mark)
			return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
					 ci->mark, selection, 2);
	}
	if (strcmp(ci->str, "start-of-line") == 0) {
		if ((cr.m->seq < ci->mark->seq && ci->mark->seq < cr.m2->seq &&
		     !mark_same(ci->mark, cr.m2)) ||
		    (cr.m2->seq < ci->mark->seq && ci->mark->seq < cr.m->seq &&
		     !mark_same(ci->mark, cr.m)))
			return comm_call(ci->comm2, "attr:cb", ci->focus, 2000000,
					ci->mark, selection, 2);
	}
	return 0;
}

DEF_CMD(emacs_hl_attrs)
{
	struct highlight_info *hi = ci->home->data;

	if (!ci->str)
		return 0;

	if (strcmp(ci->str, "render:search") == 0) {
		/* Current search match -  "20" is a priority */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int  len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:red,inverse,focus,vis-nl", 20);
		}
	}
	if (strcmp(ci->str, "render:search2") == 0) {
		/* alternate matches in current view */
		if (hi->view >= 0 && ci->mark && ci->mark->viewnum == hi->view) {
			int len = atoi(ci->str2);
			return comm_call(ci->comm2, "attr:callback", ci->focus, len,
					 ci->mark, "fg:blue,inverse,vis-nl", 20);
		}
	}
	if (strcmp(ci->str, "start-of-line") == 0 && ci->mark && hi->view >= 0) {
		struct mark *m = vmark_at_or_before(ci->focus, ci->mark, hi->view, ci->home);
		if (m && attr_find_int(m->attrs, "render:search") > 0)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 5000,
					 ci->mark, "fg:red,inverse,vis-nl", 20);
		if (m && attr_find_int(m->attrs, "render:search2") > 0)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 5000,
					 ci->mark, "fg:blue,inverse,vis-nl", 20);
	}
	if (strcmp(ci->str, "render:search-end") ==0) {
		/* Here endeth the match */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:red,inverse,vis-nl", 20);
	}
	if (strcmp(ci->str, "render:search2-end") ==0) {
		/* Here endeth the match */
		return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
				 ci->mark, "fg:blue,inverse,vis-nl", 20);
	}
	return 0;
}

DEF_CMD(emacs_goto_line)
{
	if (ci->num == NO_NUMERIC)
		return 1;
	call("CountLines", ci->focus, ci->num, ci->mark, "goto:line");
	call("Move-View-Pos", ci->focus, 0, ci->mark);
	return 1;
}

DEF_CMD(emacs_next_match)
{
	call("Mode:set-num2", ci->focus, N2_match);
	call("Message:modal", ci->focus, 0, NULL, "Type ` to search again");
	return call("interactive-cmd-next-match", ci->focus, ci->num);
}

DEF_CMD(emacs_match_again)
{
	if (N2(ci) != N2_match)
		return emacs_insert_func(ci);
	else
		return emacs_next_match_func(ci);
}

DEF_CMD(emacs_make)
{
	call("interactive-cmd-make", ci->focus,
	     strcmp(ci->key, "emCC-C-Chr-M") == 0, ci->mark);
	return 1;
}

DEF_CMD(emacs_press)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);

	if (mk) {
		struct mark *p = call_ret(mark, "doc:point", ci->focus);
		attr_set_int(&mk->attrs, "emacs:active", 0);
		attr_set_str(&mk->attrs, "emacs:selection-type", "char");
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
	}
	call("Move-CursorXY", ci->focus,
	     0, NULL, NULL, 0, NULL, NULL, ci->x, ci->y);
	call("Move-to", ci->focus, 1);
	pane_focus(ci->focus);
	return 1;
}

DEF_CMD(emacs_release)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	char *seltype = "";

	call("Move-CursorXY", ci->focus,
	     0, NULL, NULL, 0, NULL, NULL, ci->x, ci->y);

	if (mk)
		seltype = attr_find(mk->attrs, "emacs:selection-type");
	if (mk && p && seltype && strcmp(seltype, "word") == 0) {
		if (mk->seq < p->seq) {
			call("Move-Word", ci->focus, -1,  mk);
			call("Move-Word", ci->focus, 1, p);
		} else {
			call("Move-Word", ci->focus, -1,  p);
			call("Move-Word", ci->focus, 1, mk);
		}
	}
	if (mk && p && !mark_same(mk, p)) {
		char *str;

		attr_set_int(&mk->attrs, "emacs:active", 2);
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
		str = call_ret(strsave, "doc:get-str", ci->focus,
			       0, p, NULL,
			       0, mk);
		if (str && *str)
			call("copy:save", ci->focus, 0, NULL, str, 1);
	}
	return 1;
}

DEF_CMD(emacs_dpress)
{
	/* Switch to word-based selection */
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);

	if (mk)
		attr_set_str(&mk->attrs, "emacs:selection-type", "word");
	return 1;
}

DEF_CMD(emacs_paste)
{
	char *str = call_ret(strsave, "copy:get", ci->focus);

	call("Message", ci->focus, 0, NULL, "str");

	call("Move-CursorXY", ci->focus,
	     0, NULL, NULL, 0, NULL, NULL, ci->x, ci->y);

	if (!str || !*str)
		return 1;

	call("Replace", ci->focus, 0, NULL, str);

	pane_focus(ci->focus);

	return 1;
}

DEF_CMD(emacs_readonly)
{
	char *ro;

	ro = pane_attr_get(ci->focus, "doc-readonly");

	if (ro && strcmp(ro,"yes") == 0)
		call("doc:set:readonly", ci->focus, 0);
	else
		call("doc:set:readonly", ci->focus, 1);
	return 1;
}

DEF_CMD(emacs_curs_pos)
{
	struct mark *c;
	int col = 0;
	int chars = 0;
	wint_t ch, nxt;
	char *msg = NULL;

	if (!ci->mark)
		return Enoarg;
	c = mark_dup(ci->mark);
	nxt = doc_following_pane(ci->focus, c);

	while ((ch = mark_prev_pane(ci->focus, c)) != WEOF && !is_eol(ch))
		;
	while (mark_ordered_not_same(c, ci->mark)) {
		ch = mark_next_pane(ci->focus, c);
		if (is_eol(ch)) {
			col = 0;
			chars = 0;
		} else if (ch == '\t') {
			col = (col|7)+1;
			chars += 1;
		} else {
			col += 1;
			chars += 1;
		}
	}
	mark_free(c);
	asprintf(&msg, "Cursor at column %d (%d chars), char is %d (0x%x)",
		 col, chars, nxt, nxt);
	call("Message", ci->focus, 0, NULL, msg);
	free(msg);
	return 1;
}

DEF_CMD(emacs_fill)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	struct pane *p2;

	if (mk && attr_find_int(mk->attrs, "emacs:active") >= 1) {
		/* Clear current highlight */
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
		attr_set_int(&mk->attrs, "emacs:active", 0);
	} else
		mk = NULL;

	if (call("fill-paragraph", ci->focus, ci->num, p, NULL, 0, mk) == 0) {
		p2 = call_ret(pane, "attach-textfill", ci->focus);
		if (p2)
			call("fill-paragraph", p2, ci->num, p, NULL, 0, mk);
	}
	return 1;
}

DEF_CMD(emacs_abbrev)
{
	call("attach-abbrev", ci->focus);
	return 1;
}

DEF_PFX_CMD(meta_cmd, "M-");
DEF_PFX_CMD(cx_cmd, "emCX-");
DEF_PFX_CMD(cx4_cmd, "emCX4-");
DEF_PFX_CMD(cx5_cmd, "emCX5-");
DEF_PFX_CMD(cc_cmd, "emCC-");
DEF_PFX_CMD(quote_cmd, "emQ-");

static void emacs_init(void)
{
	unsigned i;
	struct map *m = key_alloc();

	key_add(m, "ESC", &meta_cmd.c);
	key_add(m, "C-Chr-X", &cx_cmd.c);
	key_add(m, "emCX-Chr-4", &cx4_cmd.c);
	key_add(m, "emCX-Chr-5", &cx5_cmd.c);
	key_add(m, "C-Chr-C", &cc_cmd.c);
	key_add(m, "C-Chr-Q", &quote_cmd.c);

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
	key_add(m, "Enter", &emacs_insert_other);
	key_add(m, "C-Chr-O", &emacs_insert_other);
	key_add(m, "Interactive:insert", &emacs_interactive_insert);
	key_add(m, "Interactive:delete", &emacs_interactive_delete);

	key_add(m, "C-Chr-_", &emacs_undo);
	key_add(m, "emCX-Chr-u", &emacs_undo);
	key_add(m, "C-Chr-/", &emacs_undo);

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

	key_add(m, "emCX-C-Chr-v", &emacs_revisit);

	key_add(m, "emCX-Chr-=", &emacs_curs_pos);

	key_add(m, "C-Chr-S", &emacs_start_search);
	key_add(m, "C-Chr-R", &emacs_start_search);
	key_add(m, "render:reposition", &emacs_reposition);

	key_add(m, "emCX-C-Chr-C", &emacs_exit);

	key_add(m, "C-Chr-U", &emacs_prefix);

	key_add(m, "M-Chr-!", &emacs_shell);
	key_add(m, "Shell Command", &emacs_shell);

	key_add(m, "emCX-Chr-`", &emacs_next_match);
	key_add(m, "Chr-`", &emacs_match_again);

	key_add_range(m, "M-Chr-0", "M-Chr-9", &emacs_num);
	key_add(m, "M-Chr--", &emacs_neg);
	key_add(m, "C-Chr--", &emacs_neg);
	key_add(m, "C-Chr- ", &emacs_mark);
	key_add(m, "mode-set-mark", &emacs_mark);
	key_add(m, "emCX-C-Chr-X", &emacs_swap_mark);
	key_add(m, "Abort", &emacs_abort);
	key_add(m, "C-Chr-W", &emacs_wipe);
	key_add(m, "M-Chr-w", &emacs_copy);
	key_add(m, "C-Chr-Y", &emacs_yank);
	key_add(m, "M-Chr-y", &emacs_yank_pop);
	key_add(m, "map-attr", &emacs_attrs);

	key_add(m, "M-Chr-g", &emacs_goto_line);
	key_add(m, "M-Chr-x", &emacs_command);
	key_add(m, "emCC-Chr-m", &emacs_make);
	key_add(m, "emCC-C-Chr-M", &emacs_make);

	key_add(m, "M-C-Chr-V", &emacs_move_view_other);

	key_add(m, "emCX-C-Chr-Q", &emacs_readonly);

	key_add_prefix(m, "emQ-Chr-", &emacs_quote_insert);
	key_add_prefix(m, "emQ-C-Chr-", &emacs_quote_insert);

	key_add(m, "M-Chr-q", &emacs_fill);
	key_add(m, "M-Chr-/", &emacs_abbrev);

	key_add(m, "emacs:command", &emacs_do_command);
	key_add(m, "interactive-cmd-version", &emacs_version);

	key_add(m, "Press-1", &emacs_press);
	key_add(m, "Release-1", &emacs_release);
	key_add(m, "DPress-1", &emacs_dpress);
	key_add(m, "Click-2", &emacs_paste);

	emacs_map = m;

	m = key_alloc();
	key_add(m, "Search String", &emacs_search_done);
	key_add(m, "render:reposition", &emacs_search_reposition);
	key_add(m, "search:highlight", &emacs_search_highlight);
	key_add(m, "map-attr", &emacs_hl_attrs);
	key_add(m, "Draw:text", &highlight_draw);
	key_add(m, "Close", &emacs_highlight_close);
	key_add(m, "Abort", &emacs_highlight_abort);
	key_add(m, "Notify:clip", &emacs_highlight_clip);
	hl_map = m;
}

DEF_LOOKUP_CMD(mode_emacs, emacs_map);

DEF_CMD(attach_mode_emacs)
{
	return call_comm("global-set-keymap", ci->focus, &mode_emacs.c);
}

DEF_CMD(attach_file_entry)
{
	pane_register(ci->focus, 0, &find_handle.c, ci->str ?: "shellcmd");
	return 1;
}

void emacs_search_init(struct pane *ed safe);
void edlib_init(struct pane *ed safe)
{
	if (emacs_map == NULL)
		emacs_init();
	if (fh_map == NULL)
		findmap_init();
	call_comm("global-set-command", ed, &attach_mode_emacs, 0, NULL, "attach-mode-emacs");
	call_comm("global-set-command", ed, &attach_file_entry, 0, NULL, "attach-file-entry");
	emacs_search_init(ed);
}
