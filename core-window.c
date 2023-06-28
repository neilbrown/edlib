/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Core per-window functionality.
 *
 * Provide a pane that is instantiated between the root and any window
 * stack, to provide common functionality.  These includes:
 *
 * - registering and forwarding per-window notifications
 * - Being an intermediary for per-window selections.
 *
 * ==============================================================
 * Allow any pane to "claim ownership" of "the selection", or to
 * "commit" the selection.  A pane can also "discard" the selection,
 * but that only works if the pane owns it.
 *
 * This can be used for mouse-based copy/paste and interaction with the
 * X11 "PRIMARY" clipboard.
 * When a selection is made in any pane it claims "the selection".
 * When a mouse-based paste request is made, the receiving pane can ask for
 * the selection to be "commited", and then access the most recent copy-buffer.
 * The owner of a selection will, if the selection is still valid, call
 * copy:save to save the selected content.
 * When a "paste" request is made where the location is based on the "point"
 * (current cursor) it is unlikely that a selection in the same pane should be
 * used - if there is one it is more likely to be intended to receive the paste.
 * So the target pane can first "discard" the selection, then "commit", then call
 * "copy:get".  If the selection is in this pane, the "discard" will succeed,
 * the "commit" will be a no-op, and the top copy buf will be used.
 * If the selection is in another pane (or another app via X11), the "discard"
 * will fail (wrong owner), the "commit" will succeed and copy the selection,
 * and the "copy:get" will get it.
 *
 * Operations are "selection:claim", "selection:commit" and "selection:discard".
 * When the selection is claimed, the old owner gets called (not notified)
 * "Notify:selection:claimed", and when a commit request is made,
 * "Notify:selection:commit" is sent.
 *
 * A client can declare itself to be a fall-back handler by calling
 * select:claim with num==1.  Then if any other client discards its selection,
 * the ownership reverse to the fallback.  The fallback typically provides
 * access to some selection external to edlib, such as the x11 selections.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

#include "core.h"
#include "internal.h"

struct window_data {
	struct pane	*sel_owner;
	int		sel_committed;
	struct pane	*sel_owner_fallback;
};

DEF_CMD(request_notify)
{
	pane_add_notify(ci->focus, ci->home, ksuffix(ci, "window:request:"));
	return 1;
}

DEF_CMD(send_notify)
{
	/* window:notify:... */
	return home_pane_notify(ci->home, ksuffix(ci, "window:notify:"),
				ci->focus,
				ci->num, ci->mark, ci->str,
				ci->num2, ci->mark2, ci->str2, ci->comm2);
}

DEF_CMD(selection_claim)
{
	struct window_data *wd = ci->home->data;

	if (wd->sel_owner && wd->sel_owner != ci->focus) {
		call("Notify:selection:claimed", wd->sel_owner);
		//pane_drop_notifiers(ci->home, "Notify:Close", wd->sel_owner);
	}
	wd->sel_owner = ci->focus;
	if (ci->num == 1)
		wd->sel_owner_fallback = ci->focus;
	wd->sel_committed = 0;
	pane_add_notify(ci->home, ci->focus, "Notify:Close");
	return 1;
}

DEF_CMD(selection_commit)
{
	struct window_data *wd = ci->home->data;

	if (wd->sel_owner && !wd->sel_committed) {
		if (call("Notify:selection:commit", wd->sel_owner) != 2)
			wd->sel_committed = 1;
	}
	return 1;
}

DEF_CMD(selection_discard)
{
	struct window_data *wd = ci->home->data;
	struct pane *op, *fp;

	if (!wd->sel_owner)
		return Efalse;
	if (wd->sel_owner_fallback == ci->focus)
		wd->sel_owner_fallback = NULL;
	/* Don't require exactly same pane for sel_owner,
	 * but ensure they have the same focus.
	 */
	op = pane_leaf(wd->sel_owner);
	fp = pane_leaf(ci->focus);
	if (fp != op)
		return Efalse;

	wd->sel_owner = wd->sel_owner_fallback;
	wd->sel_committed = 0;
	return 1;
}

DEF_CMD(close_notify)
{
	struct window_data *wd = ci->home->data;

	if (wd->sel_owner_fallback == ci->focus)
		wd->sel_owner_fallback = NULL;

	if (wd->sel_owner == ci->focus)
		wd->sel_owner = wd->sel_owner_fallback;
	return 1;
}

static struct map *window_map;
DEF_LOOKUP_CMD(window_handle, window_map);

DEF_CMD(window_attach)
{
	struct window_data *wd;
	struct pane *p;

	alloc(wd, pane);
	p = pane_register(ci->focus, 0, &window_handle.c, wd);
	if (!p) {
		unalloc(wd, pane);
		return Efail;
	}
	comm_call(ci->comm2, "cb", p);
	return 1;
}

void window_setup(struct pane *ed safe)
{
	window_map = key_alloc();

	key_add_prefix(window_map, "window:request:", &request_notify);
	key_add_prefix(window_map, "window:notify:", &send_notify);

	key_add(window_map, "selection:claim", &selection_claim);
	key_add(window_map, "selection:commit", &selection_commit);
	key_add(window_map, "selection:discard", &selection_discard);
	key_add(window_map, "Notify:Close", &close_notify);

	call_comm("global-set-command", ed, &window_attach, 0, NULL,
		  "attach-window-core");
}
