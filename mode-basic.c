/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Define basic key/mouse interactions that conform to
 * CUA.  All special modes should build on this.
 *
 */
#include <unistd.h>
#include <stdlib.h>
#include <wctype.h>


#define PANE_DATA_VOID
#include "core.h"

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
	N2_close_others,/* Last command was close-other, just a 1 can repeat */
	N2_runmacro,	/* Last command was CX-e, just an 'e' can repeat */
	N2_shift,	/* Last command was CX-< or CX-> */
	N2_growx,	/* Last command was CX-{ or CX-} */
	N2_uniquote,	/* Last command was :C-q inserting a unicode from name */
};
static inline int N2(const struct cmd_info *ci safe)
{
	return ci->num2 & 0xffff;
}

static inline int N2a(const struct cmd_info *ci safe)
{
	return ci->num2 >> 16;
}


/* selection:active encodes 4 different states for the selection
 * 0 - inactive.  The other-end might exist but it is passive, not displayed
 *                and not used unless explicitly asked for (Cx Cx)
 * 1 - active.    Selection is active and will remain active if cursor
 *                moves or text is changed.  Is used in various ways.
 * 2 - transient. Is active, but will be canceled on movement or change.
 *                Will be used for copy/cut, but little else
 * 3 - replacable Transient plus text entry will delete selected content.
 */

static void set_selection(struct pane *p safe, struct mark *pt,
			  struct mark *mk, int type)
{
	int active;
	if (!type || !mk)
		return;
	active = attr_find_int(mk->attrs, "selection:active");
	if (active == type)
		return;
	attr_set_int(&mk->attrs, "selection:active", type);
	if (!pt)
		pt = call_ret(mark, "doc:point", p);
	if (!pt)
		return;
	if (active <= 0)
		attr_set_int(&pt->attrs, "selection:active", 1);
	if (!mark_same(pt, mk))
		call("view:changed", p, 0, pt, NULL, 0, mk);
}

DEF_CMD(basic_selection_set)
{
	set_selection(ci->focus, ci->mark2, ci->mark, ci->num);
	return 1;
}

static bool clear_selection(struct pane *p safe, struct mark *pt,
			    struct mark *mk, int type)
{
	int active;
	if (!mk)
		return False;
	active = attr_find_int(mk->attrs, "selection:active");
	if (active <= 0)
		return False;
	if (type && active < type)
		return False;
	attr_set_int(&mk->attrs, "selection:active", 0);
	if (!pt)
		pt = call_ret(mark, "doc:point", p);
	if (!pt)
		return True;
	attr_set_int(&pt->attrs, "selection:active", 0);
	if (!mark_same(pt, mk))
		call("view:changed", p, 0, pt, NULL, 0, mk);
	return True;
}

DEF_CMD(basic_selection_clear)
{
	if (clear_selection(ci->focus, ci->mark2, ci->mark, ci->num))
		return 1;
	else
		return Efalse;
}

static void update_sel(struct pane *p safe,
		       struct mark *pt safe, struct mark *m2 safe,
		       const char *type)
{
	struct mark *mfirst, *mlast;
	struct mark *mk;

	call("Move-to", p, 1, m2);
	mk = call_ret(mark2, "doc:point", p);
	if (!mk)
		return;
	if (!type)
		type = attr_find(m2->attrs, "selection-type");
	else
		attr_set_str(&m2->attrs, "selection-type", type);

	if (type && strcmp(type, "char") != 0) {

		if (pt->seq < mk->seq) {
			mfirst = pt;
			mlast = mk;
		} else {
			mfirst = mk;
			mlast = pt;
		}
		if (strcmp(type, "word") == 0) {
			wint_t wch = doc_prior(p, mfirst);
			/* never move back over spaces */
			if (wch != WEOF && !iswspace(wch))
				call("doc:word", p, -1,  mfirst);
			wch = doc_following(p, mlast);
			/* For forward over a single space is OK */
			if (wch != WEOF && iswspace(wch))
				doc_next(p, mlast);
			else
				call("doc:word", p, 1, mlast);
		} else {
			call("doc:EOL", p, -1,  mfirst);
			/* Include trailing newline */
			call("doc:EOL", p, 1, mlast, NULL, 1);
		}
	}

	/* Don't set selection until range is non-empty, else we
	 * might clear some other selection too early.
	 */
	if (!mark_same(pt, mk)) {
		/* Must call 'claim' first as it might be claiming from us */
		call("selection:claim", p);
		set_selection(p, pt, mk, 2);
	}
}

DEF_CMD(basic_press)
{
	/* The second mark (managed by core-doc) is used to record the
	 * selected starting point.  When double- or triple- click
	 * asks for word or line selection, the actually start, which
	 * is stored in the first mark, may be different.
	 * That selected starting point will record the current unit
	 * siez in the emacs:selection-type attribute.
	 */
	struct mark *pt = call_ret(mark, "doc:point", ci->focus);
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *m2 = call_ret(mark2, "doc:point", ci->focus, 2);
	struct mark *m = mark_new(ci->focus);
	char *type;

	if (!m || !pt) {
		/* Not in document, not my problem */
		mark_free(m);
		return Efallthrough;
	}
	/* NOTE must find new location before view changes. */
	call("Move-CursorXY", ci->focus, 0, m, "prepare",
	     0, NULL, NULL, ci->x, ci->y);

	clear_selection(ci->focus, pt, mk, 0);
	call("Move-to", ci->focus, 0, m);
	pane_take_focus(ci->focus);

	if (m2 && strcmp(ci->key, "M:DPress-1") == 0) {
		type = attr_find(m2->attrs, "emacs:selection-type");
		if (!type)
			type = "char";
		else if (strcmp(type, "char") == 0)
			type = "word";
		else if (strcmp(type, "word") == 0)
			type = "line";
		else
			type = "char";
	} else {
		type = "char";
		/* Record start of selection */
		call("Move-to", ci->focus, 2, m);
		m2 = call_ret(mark2, "doc:point", ci->focus, 2);
		if (m2)
			attr_set_str(&m2->attrs, "emacs:selection-type", type);
	}
	if (m2) {
		/* Record co-ordinate of start so we can tell if the mouse moved. */
		attr_set_int(&m2->attrs, "emacs:track-selection",
			     1 + ci->x * 10000 + ci->y);
		update_sel(ci->focus, pt, m2, type);
	}
	mark_free(m);

	return 1;
}

DEF_CMD(basic_release)
{
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *m2 = call_ret(mark2, "doc:point", ci->focus, 2);
	struct mark *m = mark_new(ci->focus);
	char *type;
	int prev_pos;
	int moved;

	if (!p || !m2 || !m) {
		/* Not in a document or no selection start - not my problem */
		mark_free(m);
		return Efallthrough;
	}

	prev_pos = attr_find_int(m2->attrs, "emacs:track-selection");
	type = attr_find(m2->attrs, "emacs:selection-type");
	moved = prev_pos != (1 + ci->x * 10000 + ci->y);
	attr_set_int(&m2->attrs, "emacs:track-selection", 0);

	call("Move-CursorXY", ci->focus,
	     0, m, "activate", 0, NULL, NULL, ci->x, ci->y);
	/* That action might have closed a pane.  Better check... */
	if (ci->focus->damaged & DAMAGED_CLOSED) {
		/* Do nothing */
	} else if (moved) {
		/* Moved the mouse, so new location is point */
		call("Move-to", ci->focus, 0, m);
		update_sel(ci->focus, p, m2, NULL);
	} else if (type && strcmp(type, "char") != 0) {
		/* Otherwise use the old location.  Point might not
		 * be there exactly if it was moved to end of word/line
		 */
		call("Move-to", ci->focus, 0, m2);
		update_sel(ci->focus, p, m2, NULL);
	} else
		clear_selection(ci->focus, p, mk, 0);

	mark_free(m);

	return 1;
}

DEF_CMD(basic_menu_open)
{
	/* If there is a menu action here, activate it. */
	/* Don't move the cursor though */
	struct mark *m = mark_new(ci->focus);
	int ret;

	ret = call("Move-CursorXY", ci->focus, 0, m, "menu",
		   0, NULL, NULL, ci->x, ci->y);
	mark_free(m);
	return ret;
}

DEF_CMD(basic_menu_select)
{
	/* If a menu was opened it should have claimed the mouse focus
	 * so ci->focus is now the menu.  We want to activate the entry
	 * under the mouse
	 */
	struct mark *m = mark_new(ci->focus);
	int ret;

	ret = call("Move-CursorXY", ci->focus, 0, m, "activate",
		   0, NULL, NULL, ci->x, ci->y);
	mark_free(m);
	return ret;
}

DEF_CMD(basic_motion)
{
	struct mark *p = call_ret(mark, "doc:point", ci->focus);
	struct mark *m2 = call_ret(mark2, "doc:point", ci->focus, 2);

	if (!p || !m2)
		return Enoarg;

	if (attr_find_int(m2->attrs, "emacs:track-selection") <= 0)
		return Efallthrough;

	call("Move-CursorXY", ci->focus,
	     0, NULL, NULL, 0, NULL, NULL, ci->x, ci->y);

	update_sel(ci->focus, p, m2, NULL);
	return 1;
}

DEF_CMD(basic_paste_direct)
{
	/* This command is an explicit paste command and the content
	 * is available via "Paste:get".
	 * It might come via the mouse (with x,y) or via a keystroke.
	 */
	char *s;
	if (ci->key[0] == 'M') {
		call("Move-CursorXY", ci->focus,
		     0, NULL, NULL, 0, NULL, NULL, ci->x, ci->y);
		pane_take_focus(ci->focus);
	}

	s = call_ret(str, "Paste:get", ci->focus);
	if (s && *s) {
		struct mark *pt = call_ret(mark, "doc:point", ci->focus);
		struct mark *mk;
		call("Move-to", ci->focus, 1);
		mk = call_ret(mark2, "doc:point", ci->focus);
		call("Replace", ci->focus, 0, mk, s, 0, pt);
		set_selection(ci->focus, pt, mk, 2);
	}
	free(s);
	return 1;
}


DEF_CMD(basic_attrs)
{
	struct call_return cr;
	int active;
	char *selection = "bg:white-80,vis-nl,menu-at-mouse,action-menu:emacs:selection-menu"; // grey

	if (!ci->str)
		return Enoarg;

	cr = call_ret(all, "doc:point", ci->focus);
	if (cr.ret <= 0 || !cr.m || !cr.m2 || !ci->mark)
		return 1;
	active = attr_find_int(cr.m2->attrs, "selection:active");
	if (active <= 0)
		return 1;
	if (active >= 3) /* replacable */
		selection = "bg:red+80,vis-nl"; // pink
	if (mark_same(cr.m, cr.m2))
		return 1;
	if (strcmp(ci->str, "render:interactive-mark") == 0) {
		if (ci->mark == cr.m2 && cr.m2->seq < cr.m->seq)
			return comm_call(ci->comm2, "attr:callback", ci->focus, 0,
					 ci->mark, selection, 210);
		if (ci->mark == cr.m2)
			return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
					 ci->mark, selection, 210);
	}
	if (strcmp(ci->str, "render:interactive-point") == 0) {
		if (cr.m == ci->mark && cr.m->seq < cr.m2->seq)
			return comm_call(ci->comm2, "attr:cb", ci->focus, 0,
					 ci->mark, selection, 210);
		if (cr.m == ci->mark)
			return comm_call(ci->comm2, "attr:callback", ci->focus, -1,
					 ci->mark, selection, 210);
	}
	if (strcmp(ci->str, "start-of-line") == 0) {
		if ((cr.m->seq < ci->mark->seq && ci->mark->seq < cr.m2->seq &&
		     !mark_same(ci->mark, cr.m2)) ||
		    (cr.m2->seq < ci->mark->seq && ci->mark->seq < cr.m->seq &&
		     !mark_same(ci->mark, cr.m)))
			return comm_call(ci->comm2, "attr:cb", ci->focus, 0,
					 ci->mark, selection, 210);
	}
	return Efallthrough;
}

DEF_CMD(basic_selection_menu)
{
	struct pane *p;

	p = call_ret(pane, "attach-menu", ci->focus, 0, NULL, "V", 0, NULL,
		     "emacs:selection-menu-action", ci->x, ci->y+1);
	if (!p)
		return Efail;
	call("global-multicall-selection-menu:add-", p);
	call("menu-add", p, 0, NULL, "de-select", 0, NULL, ":ESC");
	return 1;
}

DEF_CMD(basic_selection_menu_action)
{
	struct pane *home = ci->home;
	const char *c = ci->str;

	if (!c)
		return 1;
	if (*c == ' ') {
		/* command for focus */
		call(c+1, ci->focus, 0, ci->mark);
		return 1;
	}

	call("Keystroke-sequence", home, 0, NULL, c);
	return 1;
}

DEF_CMD(basic_abort)
{
	/* On abort, forget mark */
	struct mark *m = call_ret(mark2, "doc:point", ci->focus);

	clear_selection(ci->focus, NULL, m, 0);
	return Efallthrough;
}

DEF_CMD(basic_sel_claimed)
{
	/* Should possibly just change the color of our selection */
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);

	clear_selection(ci->focus, NULL, mk, 0);
	return 1;
}

DEF_CMD(basic_sel_commit)
{
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	struct mark *p = call_ret(mark, "doc:point", ci->focus);

	if (p && mk && !mark_same(p, mk)) {
		char *str;

		str = call_ret(strsave, "doc:get-str", ci->focus,
			       0, p, NULL,
			       0, mk);
		if (str && *str)
			call("copy:save", ci->focus, 0, NULL, str);
	}

	return 1;
}

DEF_CMD(basic_insert)
{
	int ret;
	const char *str;
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	char dc[20];
	bool first = N2(ci) != N2_undo_insert;

	if (!ci->mark)
		return Enoarg;

	if (clear_selection(ci->focus, NULL, mk, 3)) {
		call("Replace", ci->focus, 1, mk, NULL, !first);
		first = False;
	} else
		clear_selection(ci->focus, NULL, mk, 2);

	str = ksuffix(ci, "K-");
	/* Resubmit as doc:char-$str.  By default this will be inserted
	 * but panes like lib-viewer might have other plans.
	 * lib-viewer could catch the original "K-", but sometimes
	 * the major mode might not want that.
	 */
	strcat(strcpy(dc, "doc:char-"), str);
	ret = call(dc, ci->focus, ci->num, ci->mark, NULL, !first);
	call("Mode:set-num2", ci->focus, N2_undo_insert);

	return ret < 0 ? ret : 1;
}

static struct {
	char *key;
	char *insert;
} other_inserts[] = {
	{"K:Tab", "\t"},
	{"K:LF", "\n"},
	{"K:Enter", "\n"},
	{NULL, NULL}
};

DEF_CMD(basic_insert_other)
{
	int ret;
	int i;
	struct mark *m = NULL;
	struct mark *mk = call_ret(mark2, "doc:point", ci->focus);
	bool first = N2(ci) != N2_undo_insert;
	char *ins;

	if (!ci->mark)
		return Enoarg;

	for (i = 0; other_inserts[i].key; i++)
		if (strcmp(safe_cast other_inserts[i].key, ci->key) == 0)
			break;
	ins = other_inserts[i].insert;
	if (ins == NULL)
		return Efallthrough;

	if (clear_selection(ci->focus, NULL, mk, 3)) {
		call("Replace", ci->focus, 1, mk, NULL, !first);
		first = False;
	} else
		clear_selection(ci->focus, NULL, mk, 2);

	if (!*ins) {
		ins++;
		m = mark_dup(ci->mark);
		/* Move m before ci->mark, so it doesn't move when we insert */
		mark_step(m, 0);
	}

	ret = call("Replace", ci->focus, 1, m, ins, !first, ci->mark);
	if (m) {
		mark_to_mark(ci->mark, m);
		mark_free(m);
	}
	/* A newline starts a new undo */
	call("Mode:set-num2", ci->focus, (*ins == '\n') ? 0 : N2_undo_insert);
	return ret < 0 ? ret : 1;
}

DEF_CMD(basic_interactive_insert)
{
	/* If some pane want to insert text just like it was typed,
	 * it calls this, and we set up for proper undo
	 */
	int ret;
	bool first = N2(ci) != N2_undo_insert;

	if (!ci->str)
		return Enoarg;

	if (clear_selection(ci->focus, NULL, ci->mark, 3)) {
		call("Replace", ci->focus, 1, ci->mark, NULL, !first);
		first = False;
	} else
		clear_selection(ci->focus, NULL, ci->mark, 2);
	ret = call("Replace", ci->focus, 1, ci->mark, ci->str,
		   !first);
	call("Mode:set-num2", ci->focus,
	     strchr(ci->str, '\n') ? 0 : N2_undo_insert);
	return ret < 0 ? ret : 1;
}

DEF_CMD(basic_interactive_delete)
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
	return ret < 0 ? ret : 1;
}

DEF_CMD(basic_close)
{
	call("Tile:close", ci->focus);
	return 1;
}

DEF_CMD(basic_refresh)
{
	call("Window:refresh", ci->focus);
	return 1;
}

static struct map *basic_map;
DEF_LOOKUP_CMD(mode_basic, basic_map);

DEF_PFX_CMD(help_cmd, ":Help");

static void basic_init(void)
{
	struct map *m;

	m = key_alloc();

	/* Some Function keys that CUA defines */
	key_add(m, "K:F1", &help_cmd.c);
	key_add(m, "K:F4", &basic_close);
	key_add(m, "K:F5", &basic_refresh);

	key_add_range(m, "K- ", "K-~", &basic_insert);
	key_add_range(m, "K-\200", "K-\377\377\377\377", &basic_insert);
	key_add(m, "K:Tab", &basic_insert_other);
	//key_add(m, "K:LF", &basic_insert_other);
	key_add(m, "K:Enter", &basic_insert_other);
	key_add(m, "Interactive:insert", &basic_interactive_insert);
	key_add(m, "Interactive:delete", &basic_interactive_delete);

	key_add(m, "M:Press-1", &basic_press);
	key_add(m, "M:Release-1", &basic_release);
	key_add(m, "M:Press-3", &basic_menu_open);
	key_add(m, "M:Release-3", &basic_menu_select);
	key_add(m, "M:DPress-1", &basic_press);
	key_add(m, "M:Motion", &basic_motion);
	key_add(m, "K:Paste", &basic_paste_direct);
	key_add(m, "M:Paste", &basic_paste_direct);

	key_add(m, "Notify:selection:claimed", &basic_sel_claimed);
	key_add(m, "Notify:selection:commit", &basic_sel_commit);

	key_add(m, "map-attr", &basic_attrs);
	key_add(m, "emacs:selection-menu", &basic_selection_menu);
	key_add(m, "emacs:selection-menu-action", &basic_selection_menu_action);

	key_add(m, "selection:set", &basic_selection_set);
	key_add(m, "selection:clear", &basic_selection_clear);

	key_add(m, "Abort", &basic_abort);
	key_add(m, "Cancel", &basic_abort);

	basic_map = m;
}

DEF_CMD(attach_mode_basic)
{
	struct pane *p = pane_register(ci->focus, 0, &mode_basic.c);

	if (!p)
		return Efail;
	comm_call(ci->comm2, "cb", p);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	basic_init();
	call_comm("global-set-command", ed, &attach_mode_basic,
		  0, NULL, "attach-mode-basic");
}
