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

static struct map *emacs_map;
static const char * safe file_normalize(struct pane *p safe, const char *path,
					const char *initial_path);

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
	 "K:CChr-F", "KRight", NULL},
	{CMD(emacs_move), "Move-Char", -1, 0,
	 "K:CChr-B", "KLeft", NULL},
	{CMD(emacs_move), "Move-Word", 1, 0,
	 "K:MChr-f", "K:MRight", NULL},
	{CMD(emacs_move), "Move-Word", -1, 0,
	 "K:MChr-b", "K:MLeft", NULL},
	{CMD(emacs_move), "Move-Expr", 1, 0,
	 "K:M:CChr-F", NULL, NULL},
	{CMD(emacs_move), "Move-Expr", -1, 0,
	 "K:M:CChr-B", NULL, NULL},
	{CMD(emacs_move), "Move-Expr", -1, 1,
	 "K:M:CChr-U", NULL, NULL},
	{CMD(emacs_move), "Move-Expr", 1, 1,
	 "K:M:CChr-D", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", 1, 0,
	 "K:MChr-F", NULL, NULL},
	{CMD(emacs_move), "Move-WORD", -1, 0,
	 "K:MChr-B", NULL, NULL},
	{CMD(emacs_move), "Move-EOL", 1, 0,
	 "K:CChr-E", "KEnd", NULL},
	{CMD(emacs_move), "Move-EOL", -1, 0,
	 "K:CChr-A", "KHome", NULL},
	{CMD(emacs_move), "Move-Line", -1, 0,
	 "K:CChr-P", "KUp", NULL},
	{CMD(emacs_move), "Move-Line", 1, 0,
	 "K:CChr-N", "KDown", NULL},
	{CMD(emacs_move), "Move-File", 1, 0,
	 "K:MChr->", "K:SEnd", NULL},
	{CMD(emacs_move), "Move-File", -1, 0,
	 "K:MChr-<", "K:SHome", NULL},
	{CMD(emacs_move), "Move-View-Large", 1, 0,
	 "KNext", "K:CChr-V", "emacs-move-large-other"},
	{CMD(emacs_move), "Move-View-Large", -1, 0,
	 "KPrior", "K:MChr-v", NULL},

	{CMD(emacs_move), "Move-Paragraph", -1, 0,
	 "K:M:CChr-A", NULL, NULL},
	{CMD(emacs_move), "Move-Paragraph", 1, 0,
	 "K:M:CChr-E", NULL, NULL},

	{CMD(emacs_delete), "Move-Char", 1, 0,
	 "K:CChr-D", "KDel", "del"},
	{CMD(emacs_delete), "Move-Char", -1, 0,
	 "K:CChr-H", "KBackspace", "KDelete"},
	{CMD(emacs_delete), "Move-Word", 1, 0,
	 "K:MChr-d", NULL, NULL},
	{CMD(emacs_delete), "Move-Word", -1, 0,
	 "K:M:CChr-H", "K:MBackspace", NULL},
	{CMD(emacs_kill), "Move-EOL", 1, 0,
	 "K:CChr-K", NULL, NULL},
	{CMD(emacs_kill), "Move-Expr", 1, 0,
	 "K:M:CChr-K", NULL, NULL},

	{CMD(emacs_case), "LMove-Word", 1, 0,
	 "K:MChr-l", NULL, NULL},
	{CMD(emacs_case), "UMove-Word", 1, 0,
	 "K:MChr-u", NULL, NULL},
	{CMD(emacs_case), "CMove-Word", 1, 0,
	 "K:MChr-c", NULL, NULL},
	{CMD(emacs_case), "TMove-Char", 1, 0,
	 "K:MChr-`", NULL, NULL},

	{CMD(emacs_swap), "Move-Char", 1, 0,
	 "K:CChr-T", NULL, NULL},
	{CMD(emacs_swap), "Move-Word", 1, 0,
	 "K:MChr-t", NULL, NULL},
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
	/* If there is an 'other' pane', Send "KNext" there */
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
	{CMD(emacs_simple), "Window:next", "K:CXChr-o"},
	{CMD(emacs_simple), "Window:prev", "K:CXChr-O"},
	{CMD(emacs_simple), "Window:x+", "K:CXChr-}"},
	{CMD(emacs_simple), "Window:x-", "K:CXChr-{"},
	{CMD(emacs_simple), "Window:y+", "K:CXChr-^"},
	{CMD(emacs_simple), "Window:close-others", "K:CXChr-1"},
	{CMD(emacs_simple), "Window:split-y", "K:CXChr-2"},
	{CMD(emacs_simple), "Window:split-x", "K:CXChr-3"},
	{CMD(emacs_simple), "Window:close", "K:CXChr-0"},
	{CMD(emacs_simple), "Window:scale-relative", "K:CX:CChr-="},
	{CMD(emacs_simple_neg), "Window:scale-relative", "K:CX:CChr--"},
	{CMD(emacs_simple), "Window:bury", "K:MChr-B"},
	{CMD(emacs_simple), "Display:new", "K:CX5Chr-2"},
	{CMD(emacs_simple), "Display:close", "K:CX5Chr-0"},
	{CMD(emacs_simple), "lib-server:done", "K:CXChr-#"},
	{CMD(emacs_simple), "Abort", "K:CChr-G"},
	{CMD(emacs_simple), "NOP", "K:MChr-G"},
	{CMD(emacs_simple), "NOP", "K:CX:CChr-G"},
	{CMD(emacs_simple), "NOP", "K:CX4:CChr-G"},
	{CMD(emacs_simple), "doc:save-file", "K:CX:CChr-S"},
	/* one day, this will be "find definition", now it is same as "find any" */
	{CMD(emacs_simple_num), "interactive-cmd-git-grep", "K:CX:MChr-."},
	{CMD(emacs_simple_str), "interactive-cmd-git-grep", "K:MChr-."},
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
	const char *str;

	if (!ci->mark)
		return Enoarg;

	str = ksuffix(ci, "KChr-");
	ret = call("Replace", ci->focus, 1, ci->mark, str,
		   N2(ci) == N2_undo_insert);
	call("Mode:set-num2", ci->focus, N2_undo_insert);

	return ret;
}

DEF_CMD(emacs_quote_insert)
{
	int ret;
	char buf[2] = ".";
	const char *str;

	if (!ci->mark)
		return Enoarg;

	str = ksuffix(ci, "K:CQChr-");
	if (!str[0]) {
		str = ksuffix(ci, "K:CQ:CChr-");
		if (str[0]) {
			buf[0] = str[0] & 0x1f;
			str = buf;
		} else
			str = "??";
	}
	ret = call("Replace", ci->focus, 1, ci->mark, str,
		   N2(ci) == N2_undo_insert);
	call("Mode:set-num2", ci->focus, N2_undo_insert);

	return ret;
}

static struct {
	char *key;
	char *insert;
} other_inserts[] = {
	{"KTab", "\t"},
	{"KLF", "\n"},
	{"KEnter", "\n"},
	{"K:CChr-O", "\0\n"},
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
	    strcmp(ci->key, "KEnter") == 0 &&
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
	h.want_prev = strcmp(ci->key, "K:MChr-n") == 0;

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
	key_add(fh_map, "KTab", &find_complete);
	key_add(fh_map, "KEnter", &find_done);
	key_add(fh_map, "K:MEnter", &find_done);
	key_add(fh_map, "K:MChr-p", &find_prevnext);
	key_add(fh_map, "K:MChr-n", &find_prevnext);
}

static const char * safe file_normalize(struct pane *p safe,
					const char *path,
					const char *initial_path)
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
	const char *path;
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

	if (ksuffix(ci, "File Found")[0] == 0) {
		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2",
			     0, NULL, path);
		if (!p)
			return 0;

		if (ksuffix(ci, "K:CX4")[0]) {
			attr_set_str(&p->attrs, "prompt",
				     "Find File Other Window");
			attr_set_str(&p->attrs, "done-key",
				     "File Found Other Window");
		} else {
			attr_set_str(&p->attrs, "prompt", "Find File");
			attr_set_str(&p->attrs, "done-key",
				     "File Found This Window");
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
		struct pane *this = call_ret(pane, "ThisPane", ci->focus);
		par = home_call_ret(pane, ci->focus, "DocPane", p);
		if (!par || par == this)
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
	const char *str;
	const char *d, *b;
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

	if (ksuffix(ci, "Doc Found")[0] == 0) {
		struct pane *dflt;
		char *defname = NULL;

		dflt = call_ret(pane, "docs:choose", ci->focus);
		if (dflt)
			defname = pane_attr_get(dflt, "doc-name");

		p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2",
			     0, NULL, "");
		if (!p)
			return 0;

		if (defname)
			attr_set_str(&p->attrs, "default", defname);
		if (ksuffix(ci, "K:CX4")[0]) {
			attr_set_str(&p->attrs, "prompt",
				     "Find Document Other Window");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found Other Window");
		} else {
			attr_set_str(&p->attrs, "prompt", "Find Document");
			attr_set_str(&p->attrs, "done-key",
				     "Doc Found This Window");
		}
		call("doc:set-name", p, 0, NULL, "Find Document", -1);

		pane_register(p, 0, &find_handle.c, "doc");
		return 1;
	}

	p = call_ret(pane, "docs:byname", ci->focus, 0, NULL, ci->str);
	if (!p)
		return Efail;

	if (strcmp(ci->key, "Doc Found Other Window") == 0) {
		struct pane *this = call_ret(pane, "ThisPane", ci->focus);
		par = home_call_ret(pane, ci->focus, "DocPane", p);
		if (!par || par == this)
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
	const char *last = ksuffix(ci, "K:MChr-");
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

DEF_CMD(emacs_start_search)
{
	struct pane *p = NULL, *hp;
	int mode = 0;

	hp = call_ret(pane, "attach-emacs-search-highlight", ci->focus);

	if (hp)
		p = call_ret(pane, "PopupTile", hp, 0, NULL, "TR2",
			     0, NULL, "");

	if (!p)
		return 0;
	home_call(hp, "highlight:set-popup", p);

	attr_set_str(&p->attrs, "prompt", "Search");
	attr_set_str(&p->attrs, "done-key", "Search String");
	call("doc:set-name", p, 0, NULL, "Search", -1);
	if (strcmp(ci->key, "K:CChr-R") == 0)
		mode |= 1;
	if (strcmp(ci->key, "K:MChr-%") == 0)
		mode |= 2;
	call_ret(pane, "attach-emacs-search", p, mode);

	return 1;
}



DEF_CMD(emacs_command)
{
	struct pane *p;

	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2", 0, NULL, "");
	if (!p)
		return 0;
	attr_set_str(&p->attrs, "prompt", "Cmd");
	attr_set_str(&p->attrs, "done-key", "emacs:command");
	call("doc:set-name", p, 0, NULL, "K:Mx command", -1);
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
	     strcmp(ci->key, "K:CC:CChr-M") == 0, ci->mark);
	return 1;
}

DEF_CMD(emacs_press)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *m = vmark_new(ci->focus, MARK_UNGROUPED, NULL);
	char *type = strcmp(ci->key, "Press-1") == 0 ? "char" : "word";

	if (!m)
		return Efail;
	/* NOTE must find new location before tw report that the
	 * view has changed.
	 */
	call("Move-CursorXY", ci->focus,
	     0, m, NULL, 0, NULL, NULL, ci->x, ci->y);
	if (mk) {
		struct mark *p = call_ret(mark, "doc:point", ci->focus);
		attr_set_int(&mk->attrs, "emacs:active", 0);
		call("view:changed", ci->focus, 0, p, NULL, 0, mk);
		if (p && !mark_same(p, m))
			type = "char";
	}
	call("Move-to", ci->focus, 0, m);
	call("Move-to", ci->focus, 1, m);
	if (!mk)
		mk = call_ret(mark2, "doc:point", ci->focus);
	if (mk)
		attr_set_str(&mk->attrs, "emacs:selection-type", type);
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

DEF_PFX_CMD(meta_cmd, ":M");
DEF_PFX_CMD(cx_cmd, ":CX");
DEF_PFX_CMD(cx4_cmd, ":CX4");
DEF_PFX_CMD(cx5_cmd, ":CX5");
DEF_PFX_CMD(cc_cmd, ":CC");
DEF_PFX_CMD(quote_cmd, ":CQ");

static void emacs_init(void)
{
	unsigned i;
	struct map *m = key_alloc();

	key_add(m, "KESC", &meta_cmd.c);
	key_add(m, "K:CChr-X", &cx_cmd.c);
	key_add(m, "K:CXChr-4", &cx4_cmd.c);
	key_add(m, "K:CXChr-5", &cx5_cmd.c);
	key_add(m, "K:CChr-C", &cc_cmd.c);
	key_add(m, "K:CChr-Q", &quote_cmd.c);

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

	key_add_range(m, "KChr- ", "KChr-~", &emacs_insert);
	key_add_range(m, "KChr-\200", "KChr-\377\377\377\377", &emacs_insert);
	key_add(m, "KTab", &emacs_insert_other);
	key_add(m, "KLF", &emacs_insert_other);
	key_add(m, "KEnter", &emacs_insert_other);
	key_add(m, "K:CChr-O", &emacs_insert_other);
	key_add(m, "Interactive:insert", &emacs_interactive_insert);
	key_add(m, "Interactive:delete", &emacs_interactive_delete);

	key_add(m, "K:CChr-_", &emacs_undo);
	key_add(m, "K:CXChr-u", &emacs_undo);
	key_add(m, "K:CChr-/", &emacs_undo);

	key_add(m, "K:CChr-L", &emacs_recenter);

	key_add(m, "K:CX:CChr-F", &emacs_findfile);
	key_add(m, "K:CX4:CChr-F", &emacs_findfile);
	key_add(m, "K:CX4Chr-f", &emacs_findfile);
	key_add(m, "File Found This Window", &emacs_findfile);
	key_add(m, "File Found Other Window", &emacs_findfile);

	key_add(m, "K:CXChr-b", &emacs_finddoc);
	key_add(m, "K:CX4Chr-b", &emacs_finddoc);
	key_add(m, "Doc Found This Window", &emacs_finddoc);
	key_add(m, "Doc Found Other Window", &emacs_finddoc);
	key_add(m, "K:CX:CChr-B", &emacs_viewdocs);

	key_add(m, "K:CXChr-k", &emacs_kill_doc);

	key_add(m, "K:CXChr-s", &emacs_save_all);

	key_add(m, "K:CX:CChr-v", &emacs_revisit);

	key_add(m, "K:CXChr-=", &emacs_curs_pos);

	key_add(m, "K:CChr-S", &emacs_start_search);
	key_add(m, "K:CChr-R", &emacs_start_search);
	key_add(m, "K:MChr-%", &emacs_start_search);
	key_add(m, "render:reposition", &emacs_reposition);

	key_add(m, "K:CX:CChr-C", &emacs_exit);

	key_add(m, "K:CChr-U", &emacs_prefix);

	key_add(m, "K:MChr-!", &emacs_shell);
	key_add(m, "Shell Command", &emacs_shell);

	key_add(m, "K:CXChr-`", &emacs_next_match);
	key_add(m, "KChr-`", &emacs_match_again);

	key_add_range(m, "K:MChr-0", "K:MChr-9", &emacs_num);
	key_add(m, "K:MChr--", &emacs_neg);
	key_add(m, "K:CChr--", &emacs_neg);
	key_add(m, "K:CChr- ", &emacs_mark);
	key_add(m, "mode-set-mark", &emacs_mark);
	key_add(m, "K:CX:CChr-X", &emacs_swap_mark);
	key_add(m, "Abort", &emacs_abort);
	key_add(m, "K:CChr-W", &emacs_wipe);
	key_add(m, "K:MChr-w", &emacs_copy);
	key_add(m, "K:CChr-Y", &emacs_yank);
	key_add(m, "K:MChr-y", &emacs_yank_pop);
	key_add(m, "map-attr", &emacs_attrs);

	key_add(m, "K:MChr-g", &emacs_goto_line);
	key_add(m, "K:MChr-x", &emacs_command);
	key_add(m, "K:CCChr-m", &emacs_make);
	key_add(m, "K:CC:CChr-M", &emacs_make);

	key_add(m, "K:M:CChr-V", &emacs_move_view_other);

	key_add(m, "K:CX:CChr-Q", &emacs_readonly);

	key_add_prefix(m, "K:CQChr-", &emacs_quote_insert);
	key_add_prefix(m, "K:CQ:CChr-", &emacs_quote_insert);

	key_add(m, "K:MChr-q", &emacs_fill);
	key_add(m, "K:MChr-/", &emacs_abbrev);

	key_add(m, "emacs:command", &emacs_do_command);
	key_add(m, "interactive-cmd-version", &emacs_version);

	key_add(m, "Press-1", &emacs_press);
	key_add(m, "Release-1", &emacs_release);
	key_add(m, "DPress-1", &emacs_press);
	key_add(m, "Click-2", &emacs_paste);

	emacs_map = m;
}

DEF_LOOKUP_CMD(mode_emacs, emacs_map);

DEF_CMD(attach_mode_emacs)
{
	return call_comm("global-set-keymap", ci->focus, &mode_emacs.c);
}

DEF_CMD(attach_file_entry)
{
	pane_register(ci->focus, 0, &find_handle.c,
		      (void*) ci->str ?: "shellcmd");
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
