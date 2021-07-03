/*
 * Copyright Neil Brown ©2017-2021 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * A viewer pane presents a read-only view on a document
 * which uses some letter - that would normally self-insert -
 * to move around.
 * Particularly:
 *  SPACE : page down
 *  BACKSPACE: page up
 *  q     : bury-document
 */

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "core.h"

static struct map *viewer_map safe;
DEF_LOOKUP_CMD(viewer_handle, viewer_map);

struct viewer_data {
	bool active;
};

static struct pane *safe do_viewer_attach(struct pane *par)
{
	struct viewer_data *vd;

	alloc(vd, pane);
	vd->active = True;
	return pane_register(par, 0, &viewer_handle.c, vd);
}

DEF_CMD(viewer_attach)
{
	return comm_call(ci->comm2, "callback:attach", do_viewer_attach(ci->focus));
}

DEF_CMD(no_replace)
{
	struct viewer_data *vd = ci->home->data;

	if (!vd->active)
		return Efallthrough;
	call("Message:modal", ci->focus, 0, NULL, "Cannot modify document in viewer mode");
	return 1;
}

DEF_CMD(viewer_cmd)
{
	/* Send command to the document */
	char cmd[40];
	const char *s;
	struct viewer_data *vd = ci->home->data;

	if (!vd->active)
		return Efallthrough;

	if ((s=ksuffix(ci, "K:"))[0] ||
	    (s=ksuffix(ci, "doc:char-"))[0]) {
		int ret;
		snprintf(cmd, sizeof(cmd), "doc:cmd%s", s-1);
		ret = call(cmd, ci->focus, ci->num, ci->mark);
		switch(ret) {
		case 0:
			snprintf(cmd, sizeof(cmd),
				 "Unknown command `%s'", s);
			call("Message:modal", ci->focus, 0, NULL, cmd);
			break;
		case 2: /* request to move to next line */
			call("K:Down", ci->focus, ci->num, ci->mark);
			break;
		case 3: /* request to move to previous line */
			call("K:Up", ci->focus, ci->num, ci->mark);
			break;
		}
	}
	return 1;
}

DEF_CMD(viewer_page_down)
{
	struct viewer_data *vd = ci->home->data;

	if (!vd->active)
		return Efallthrough;
	call("K:Next", ci->focus, ci->num, ci->mark);
	return 1;
}

DEF_CMD(viewer_page_up)
{
	struct viewer_data *vd = ci->home->data;

	if (!vd->active)
		return Efallthrough;
	call("K:Prior", ci->focus, ci->num, ci->mark);
	return 1;
}

DEF_CMD(viewer_bury)
{
	/* First see if doc wants to handle 'q' */
	int ret;
	struct viewer_data *vd = ci->home->data;

	if (!vd->active)
		return Efallthrough;

	ret = call("doc:cmd-q", ci->focus, ci->num, ci->mark);
	switch (ret) {
	case 0:
		call("Window:bury", ci->focus);
		break;
	case 2: /* request to move to next line */
		call("K:Down", ci->focus, ci->num, ci->mark);
		break;
	case 3: /* request to move to previous line */
		call("K:Up", ci->focus, ci->num, ci->mark);
		break;
	}
	return 1;
}

DEF_CMD(viewer_deactivate)
{
	struct viewer_data *vd = ci->home->data;

	if (!vd->active)
		return Efallthrough;
	vd->active = False;
	return 1;
}

DEF_CMD(viewer_activate)
{
	struct viewer_data *vd = ci->home->data;

	vd->active = True;
	return 1;
}

DEF_CMD(viewer_clone)
{
	struct pane *p;
	struct viewer_data *vd = ci->home->data;

	if (vd->active)
		p = do_viewer_attach(ci->focus);
	else
		p = ci->focus;
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(viewer_appeared)
{
	char *t = pane_attr_get(ci->focus, "doc-type");
	if (t && strcmp(t, "text") == 0)
		attr_set_str(&ci->focus->attrs, "view-cmd-V", "viewer");
	return Efallthrough;
}

void edlib_init(struct pane *ed safe)
{
	viewer_map = key_alloc();

	key_add(viewer_map, "Replace", &no_replace);
	key_add_range(viewer_map, "doc:char- ", "doc:char-~", &viewer_cmd);
	key_add(viewer_map, "K:Enter", &viewer_cmd);
	key_add(viewer_map, "doc:char- ", &viewer_page_down);
	key_add(viewer_map, "K:C-H", &viewer_page_up);
	key_add(viewer_map, "K:Backspace", &viewer_page_up);
	key_add(viewer_map, "K:Del", &viewer_page_up);
	key_add(viewer_map, "doc:char-q", &viewer_bury);
	key_add(viewer_map, "doc:char-E", &viewer_deactivate);
	key_add(viewer_map, "Clone", &viewer_clone);
	key_add(viewer_map, "Free", &edlib_do_free);
	key_add(viewer_map, "attach-viewer", &viewer_activate);

	call_comm("global-set-command", ed, &viewer_attach, 0, NULL,
		  "attach-viewer");
	call_comm("global-set-command", ed, &viewer_appeared, 0, NULL,
		  "doc:appeared-viewer");

	/* FIXME this doesn't seem quite right...
	 * The goal is that if 'viewer' is requested of doc:attach-pane,
	 * this pane gets attached, in place of any default.
	 * I'm not sure it should be "in-place", and I feel it should be easier
	 * to over-ride..
	 */
	attr_set_str(&ed->attrs, "view-viewer", "viewer");
}
