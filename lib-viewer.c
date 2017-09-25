/*
 * Copyright Neil Brown ©2017 <neil@brown.name>
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

#include "core.h"

static struct map *viewer_map safe;
DEF_LOOKUP_CMD(viewer_handle, viewer_map);

static struct pane *safe do_viewer_attach(struct pane *par)
{
	return pane_register(par, 0, &viewer_handle.c, NULL, NULL);
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

DEF_CMD(viewer_page_down)
{
	call("Next", ci->focus, ci->num, ci->mark);
	return 1;
}

DEF_CMD(viewer_page_up)
{
	call("Prior", ci->focus, ci->num, ci->mark);
	return 1;
}

DEF_CMD(viewer_bury)
{
	struct pane *tile;
	call("doc:revisit", ci->focus, -1);
	tile = call_pane("ThisPane", ci->focus);
	if (tile)
		tile = pane_my_child(tile, ci->focus);
	if (tile)
		pane_close(tile);
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
		attr_set_str(&ci->focus->attrs, "render-Chr-V", "default:viewer");
	return 0;
}

void edlib_init(struct pane *ed safe)
{
	viewer_map = key_alloc();

	key_add(viewer_map, "Replace", &no_replace);
	key_add(viewer_map, "Chr- ", &viewer_page_down);
	key_add(viewer_map, "C-Chr-H", &viewer_page_up);
	key_add(viewer_map, "Backspace", &viewer_page_up);
	key_add(viewer_map, "Del", &viewer_page_up);
	key_add(viewer_map, "Chr-q", &viewer_bury);
	key_add(viewer_map, "Chr-E", &viewer_close);
	key_add(viewer_map, "Clone", &viewer_clone);

	call_comm("global-set-command", ed, &viewer_attach, 0, NULL, "attach-viewer");
	call_comm("global-set-command", ed, &viewer_appeared, 0, NULL, "doc:appeared-viewer");
}
