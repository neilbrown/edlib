/*
 * Copyright Neil Brown ©2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * askpass - ask for a password.
 * This doesn't yet have any focus on protecting the password
 * from being swapped out.
 *
 * We place a popup in mid-display with a message and
 * "HEAVY BALLOT X" for each character typed.
 */

#include <unistd.h>
#include <memory.h>

#define PANE_DATA_TYPE struct apinfo
#include "core.h"

struct apinfo {
	char *msg safe;
	struct buf b;
	struct command *c;
};
#include "core-pane.h"

static struct map *askpass_map;
DEF_LOOKUP_CMD(askpass_handle, askpass_map);

DEF_CMD(askpass_refresh_view)
{
	struct buf b;
	struct apinfo *ai = &ci->home->data;
	int shift = 0;
	int i;

	buf_init(&b);
	buf_concat(&b, ai->msg);
	for (i = 0; i < utf8_strlen(buf_final(&ai->b)); i++)
		buf_append(&b, 0x2718); /* HEAVY BALLOT X */
	call("render-line:set", ci->focus, b.len, NULL, buf_final(&b));
	for (i = 0; i < 10; i++) {
		int cw;
		attr_set_int(&ci->focus->attrs, "shift_left", shift);
		call("render-line:measure", ci->focus, b.len);
		cw = pane_attr_get_int(ci->focus, "curs_width", 1);
		if (ci->home->parent->cx < ci->home->parent->w - cw)
			break;
		shift += 8 * cw;
	}
	free(buf_final(&b));
	return 1;
}

DEF_CMD(askpass_key)
{
	const char *k = ksuffix(ci, "K-");
	struct apinfo *ai = &ci->home->data;

	buf_concat(&ai->b, k);
	pane_damaged(ci->home, DAMAGED_VIEW);
	return 1;
}

DEF_CMD(askpass_bs)
{
	struct apinfo *ai = &ci->home->data;

	if (ai->b.len > 0)
		ai->b.len = utf8_round_len(ai->b.b, ai->b.len-1);
	pane_damaged(ci->home, DAMAGED_VIEW);
	return 1;
}

DEF_CMD(askpass_ignore)
{
	return 1;
}

DEF_CMD(askpass_done)
{
	struct apinfo *ai = &ci->home->data;

	comm_call(ai->c, "cb", ci->focus, ai->b.len, NULL,
		  buf_final(&ai->b));
	memset(ai->b.b, 0, ai->b.size);
	call("popup:close", ci->focus);
	return 1;
}

DEF_CMD(askpass_abort)
{
	struct apinfo *ai = &ci->home->data;

	memset(ai->b.b, 0, ai->b.size);
	comm_call(ai->c, "cb", ci->focus, -1);
	call("popup:close", ci->focus);
	return 1;
}

DEF_CMD(askpass_attach)
{
	struct pane *p, *p2;

	if (!ci->str || !ci->comm2)
		return Enoarg;
	p = call_ret(pane, "PopupTile", ci->focus, 0, NULL, "D2");
	if (!p)
		return Efail;
	p2  = call_ret(pane, "attach-view", p);
	if (!p2)
		goto fail;
	p = p2;

	p2 = call_ret(pane, "attach-renderline", p);
	if (!p2)
		goto fail;
	p = p2;

	p2 = pane_register(p, 0, &askpass_handle.c);
	if (!p2)
		goto fail;
	p = p2;

	attr_set_str(&p->attrs, "pane-title", "Ask Password");

	p->data.msg = strdup(ci->str);
	p->data.c = command_get(ci->comm2);
	buf_init(&p->data.b);
	pane_damaged(p, DAMAGED_VIEW);
	return 1;

fail:
	if (ci->focus->focus)
		pane_close(ci->focus->focus);
	return Efail;
}

DEF_CMD(askpass_close)
{
	struct apinfo *ai = &ci->home->data;

	free(ai->msg);
	ai->msg = safe_cast NULL;
	free(buf_final(&ai->b));
	ai->b.b = safe_cast NULL;
	command_put(ai->c);
	ai->c = NULL;
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &askpass_attach,
		  0, NULL, "AskPass");

	askpass_map = key_alloc();
	key_add(askpass_map, "Close", &askpass_close);
	key_add_prefix(askpass_map, "K-", &askpass_key);
	key_add_prefix(askpass_map, "K:", &askpass_ignore);
	key_add_prefix(askpass_map, "M:", &askpass_ignore);
	key_add(askpass_map, "K:Enter", &askpass_done);
	key_add(askpass_map, "K:Backspace", &askpass_bs);
	key_add(askpass_map, "K:ESC", &askpass_abort);
	key_add(askpass_map, "K:C-C", &askpass_abort);
	key_add(askpass_map, "K:C-G", &askpass_abort);
	key_add_prefix(askpass_map, "Refresh:view", &askpass_refresh_view);
}
