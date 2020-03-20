/*
 * Copyright Neil Brown ©2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * x11selection - integrate X11 clipboards with copybuf and selection.
 *
 * Use gtk_clipboard interfaces to provide the selection and recent copied
 * content to other applications, and to use what is provided by those
 * applications to satisfy internal requests.
 *
 * We overload copy:save to claim both PRIMARY and CLIPBOARD so other apps will
 * ask us for content.  When asked we call copy:get to get the content, but see
 * selections below.
 * We overload copy:get to interpolate PRIMARY and CLIPBOARD into the list
 * of copies, if they are exist, are not owned by us and only consider CLIPBOARD
 * if it is different to PRIMARY.
 *
 * We also claim the selection at startup on behalf of whichever application
 * owns it.  If it is claimed from us, we claim ownership of PRIMARY.
 * If it is committed, we ask for text from the owner of PRIMARY and save that.
 * If we lose ownership of the PRIMARY, we reclaim the selection.
 */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include "core.h"

#ifndef __CHECKER__
#include <gtk/gtk.h>
#else
/* sparse/smatch don't like gtk.h, so provide our own declarations */
typedef void *gpointer;
typedef unsigned int guint;
typedef char gchar;
typedef struct _GdkDisplay {} GdkDisplay;
typedef struct _GtkTargetEntry {} GtkTargetEntry;
typedef struct _GtkTargetList {} GtkTargetList;
typedef struct _GtkClipboard {} GtkClipboard;
typedef struct _GtkSelectionData {} GtkSelectionData;
typedef int GdkAtom;

GdkAtom gdk_atom_intern(gchar *, int);
#define TRUE (1)
#define FALSE (0)

GdkDisplay *gdk_display_open(gchar*);
GtkClipboard *gtk_clipboard_get_for_display(GdkDisplay*, GdkAtom);
void gtk_clipboard_set_with_data(GtkClipboard*, GtkTargetEntry*, guint,
				 void (*get)(GtkClipboard *, GtkSelectionData *,
					     guint, gpointer d safe),
				 void (*clear)(GtkClipboard *, gpointer safe),
				 gpointer d safe);
void gtk_selection_data_set_text(GtkSelectionData *, gchar *, guint);

gchar *gtk_clipboard_wait_for_text(GtkClipboard*);
int gtk_clipboard_wait_is_text_available(GtkClipboard*);
void g_free(gpointer);

GtkTargetList *gtk_target_list_new(gpointer, guint);
void gtk_target_list_add_text_targets(GtkTargetList *, guint);
GtkTargetEntry *gtk_target_table_new_from_list(GtkTargetList *, int *);
void gtk_target_list_unref(GtkTargetList *);

#endif

struct xs_info {
	struct pane		*self safe;
	struct cb {
		struct xs_info	**data;
		int		saved;
		GtkClipboard	*cb;
	} primary, clipboard;
	GtkTargetEntry		*text_targets;
	int			n_text_targets;
};

static void do_get(GtkClipboard *cb, GtkSelectionData *sd,
		   guint info, gpointer vdata safe)
{
	struct xs_info **data = vdata;
	struct xs_info *xsi = *data;
	char *s;

	if (!xsi)
		return;
	if (cb == xsi->primary.cb)
		call("selection:commit", xsi->self);

	s = call_ret(strsave, "copy:get", xsi->self);
	if (!s)
		s = "";
	gtk_selection_data_set_text(sd, s, strlen(s));
}

static void do_clear(GtkClipboard *cb, gpointer vdata safe)
{
	struct xs_info **data = vdata;
	struct xs_info *xsi = *data;

	if (!xsi)
		return;
	/* Someone else wants the clipboard */
	if (data == xsi->primary.data) {
		xsi->primary.data = NULL;
		call("selection:claim", xsi->self);
	}

	if (data == xsi->clipboard.data)
		xsi->clipboard.data = NULL;

	free(data);
}

static void claim_primary(struct xs_info *xsi safe)
{
	struct xs_info **data;

	data = malloc(sizeof(*data));
	*data = xsi;

	gtk_clipboard_set_with_data(xsi->primary.cb,
				    xsi->text_targets,
				    xsi->n_text_targets,
				    do_get, do_clear, data);
	xsi->primary.data = data;
	xsi->primary.saved = 0;
}

static void claim_both(struct xs_info *xsi safe)
{
	struct xs_info **data;

	claim_primary(xsi);

	data = malloc(sizeof(*data));
	*data = xsi;

	gtk_clipboard_set_with_data(xsi->clipboard.cb,
				    xsi->text_targets,
				    xsi->n_text_targets,
				    do_get, do_clear, data);
	xsi->clipboard.data = data;
	xsi->clipboard.saved = 0;
}

DEF_CMD(xs_copy_save)
{
	struct xs_info *xsi = ci->home->data;

	claim_both(xsi);
	call("selection:discard", ci->home);
	/* fall-through */
	return 0;
}

DEF_CMD(xs_copy_get)
{
	struct xs_info *xsi = ci->home->data;
	int num = ci->num;

	if (xsi->clipboard.data == NULL) {
		if (num == 0) {
			/* Return CLIPBOARD if it exists */
			gchar *s;

			s = gtk_clipboard_wait_for_text(xsi->clipboard.cb);
			if (s && *s) {
				comm_call(ci->comm2, "cb", ci->focus,
					  0, NULL, s);
				num -= 1;
			}
			g_free(s);
		} else {
			/* Just check if a string exists */
			if (gtk_clipboard_wait_is_text_available(
				    xsi->clipboard.cb))
				num -= 1;
		}
	}
	if (num < 0)
		return 1;

	return call_comm(ci->key, ci->home->parent, ci->comm2, num);
}

DEF_CMD(xs_sel_claimed)
{
	struct xs_info *xsi = ci->home->data;

	if (ci->focus != ci->home)
		/* not for me */
		return 0;
	/* Some other pane holds the selection, so better tell
	 * other X11 clients
	 */
	claim_primary(xsi);
	return 1;
}

DEF_CMD(xs_sel_commit)
{
	struct xs_info *xsi = ci->home->data;
	gchar *s;

	/* Someone wants to paste the selection */
	/* Record PRIMARY if it exists */

	if (ci->focus != ci->home)
		/* not for me */
		return 0;

	if (xsi->primary.data || xsi->primary.saved)
		/* We own the primary, so nothing to do */
		return 1;

	if (!xsi->clipboard.data && !xsi->clipboard.saved) {
		/* get the clipboard first - to make sure it is available
		 * as second saved text.
		 */
		s = gtk_clipboard_wait_for_text(xsi->clipboard.cb);
		if (s && *s) {
			call("copy:save", ci->home->parent,
			     0, NULL, s);
			xsi->clipboard.saved = 1;
		}
		g_free(s);
	}
	s = gtk_clipboard_wait_for_text(xsi->primary.cb);
	if (s && *s) {
		call("copy:save", ci->home->parent,
		     0, NULL, s);
		xsi->primary.saved = 1;
	}
	g_free(s);

	return 0;
}

static struct map *xs_map;
DEF_LOOKUP_CMD(xs_handle, xs_map);

DEF_CMD(xs_attach)
{
	char *d;
	struct xs_info *xsi;
	GdkAtom primary, clipboard;
	GtkTargetList *list;
	GdkDisplay *dis;

	d = pane_attr_get(ci->focus, "DISPLAY");
	if (!d || !*d)
		return 1;
	dis = gdk_display_open(d);
	if (!dis)
		return 1;

	call("attach-glibevents", ci->focus);
	alloc(xsi, pane);

	primary = gdk_atom_intern("PRIMARY", TRUE);
	clipboard = gdk_atom_intern("CLIPBOARD", TRUE);
	xsi->primary.cb = gtk_clipboard_get_for_display(dis, primary);
	xsi->clipboard.cb = gtk_clipboard_get_for_display(dis, clipboard);

	list = gtk_target_list_new(NULL, 0);
	gtk_target_list_add_text_targets(list, 0);
	xsi->text_targets =
		gtk_target_table_new_from_list(list, &xsi->n_text_targets);
	gtk_target_list_unref (list);

	claim_both(xsi);

	xsi->self = pane_register(ci->focus, 0, &xs_handle.c, xsi);
	return comm_call(ci->comm2, "cb:attach", xsi->self);
}

void edlib_init(struct pane *ed safe)
{
	if (!xs_map) {
		xs_map = key_alloc();
		key_add(xs_map, "copy:save", &xs_copy_save);
		key_add(xs_map, "copy:get", &xs_copy_get);
		key_add(xs_map, "Notify:selection:claimed", &xs_sel_claimed);
		key_add(xs_map, "Notify:selection:commit", &xs_sel_commit);
	}

	call_comm("global-set-command", ed, &xs_attach,
		  0, NULL, "attach-x11selection");
}
