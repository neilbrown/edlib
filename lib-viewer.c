/*
 * Copyright Neil Brown ©2017-2020 <neil@brown.name>
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

static struct pane *safe do_viewer_attach(struct pane *par)
{
	return pane_register(par, 0, &viewer_handle.c);
}

DEF_CMD(viewer_attach)
{
	return comm_call(ci->comm2, "callback:attach", do_viewer_attach(ci->focus));
}

DEF_CMD(no_replace)
{
	/* FIXME message? */
	return 1;
}

DEF_CMD(viewer_cmd)
{
	/* Send command to the document */
	char cmd[40];

	if (ksuffix(ci, "K:") || ksuffix(ci, "K-")) {
		int ret;
		snprintf(cmd, sizeof(cmd), "doc:cmd%s", ci->key+1);
		ret = call(cmd, ci->focus, ci->num, ci->mark);
		switch(ret) {
		case 0:
			snprintf(cmd, sizeof(cmd),
				 "Unknown command `%s'", ci->key+2);
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
	call("K:Next", ci->focus, ci->num, ci->mark);
	return 1;
}

DEF_CMD(viewer_page_up)
{
	call("K:Prior", ci->focus, ci->num, ci->mark);
	return 1;
}

DEF_CMD(viewer_bury)
{
	/* First see if doc wants to handle 'q' */
	int ret;

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

DEF_CMD(viewer_close)
{
	pane_close(ci->home);
	return 1;
}

DEF_CMD(viewer_clone)
{
	struct pane *p;
	p = do_viewer_attach(ci->focus);
	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(viewer_appeared)
{
	char *t = pane_attr_get(ci->focus, "doc-type");
	if (t && strcmp(t, "text") == 0)
		attr_set_str(&ci->focus->attrs, "view-cmd-V", "viewer");
	return 0;
}

void edlib_init(struct pane *ed safe)
{
	viewer_map = key_alloc();

	key_add(viewer_map, "Replace", &no_replace);
	key_add_range(viewer_map, "K- ", "K-~", &viewer_cmd);
	key_add(viewer_map, "K:Enter", &viewer_cmd);
	key_add(viewer_map, "K- ", &viewer_page_down);
	key_add(viewer_map, "K:C-H", &viewer_page_up);
	key_add(viewer_map, "K:Backspace", &viewer_page_up);
	key_add(viewer_map, "K:Del", &viewer_page_up);
	key_add(viewer_map, "K-q", &viewer_bury);
	key_add(viewer_map, "K-E", &viewer_close);
	key_add(viewer_map, "Clone", &viewer_clone);

	call_comm("global-set-command", ed, &viewer_attach, 0, NULL, "attach-viewer");
	call_comm("global-set-command", ed, &viewer_appeared, 0, NULL, "doc:appeared-viewer");
	/* FIXME this doesn't seem quite right...
	 * The goal is that if 'viewer' is requested of doc:attach-pane,
	 * this pane gets attached, in place of any default.
	 * I'm not sure it should be "in-place", and I feel it should be easier
	 * to over-ride..
	 */
	attr_set_str(&ed->attrs, "view-viewer", "viewer");
}
