/*
 * Copyright Neil Brown ©2020-2021 <neil@brown.name>
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
 * We also claim the edlib selection at startup on behalf of whichever X11
 * application owns it.  If it is claimed from us, we claim ownership of PRIMARY.
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
void gdk_display_close(GdkDisplay*);
GtkClipboard *gtk_clipboard_get_for_display(GdkDisplay*, GdkAtom);
void gtk_clipboard_set_with_data(GtkClipboard*, GtkTargetEntry*, guint,
				 void (*get)(GtkClipboard *, GtkSelectionData *,
					     guint, gpointer d safe),
				 void (*clear)(GtkClipboard *, gpointer safe),
				 gpointer d safe);
void gtk_clipboard_clear(GtkClipboard*);
void gtk_selection_data_set_text(GtkSelectionData *, gchar *, guint);

gchar *gtk_clipboard_wait_for_text(GtkClipboard*);
int gtk_clipboard_wait_is_text_available(GtkClipboard*);
void g_free(gpointer);

GtkTargetList *gtk_target_list_new(gpointer, guint);
void gtk_target_list_add_text_targets(GtkTargetList *, guint);
void gtk_target_list_unref(GtkTargetList *);
GtkTargetEntry *gtk_target_table_new_from_list(GtkTargetList *, int *);
void gtk_target_table_free(GtkTargetEntry*, int);

#endif

struct xs_info {
	struct pane		*self safe;
	GdkDisplay		*display;
	struct cb {
		/* 'data' is allocated space that stores a pointer to this
		 * xs_info.  Data is given the gtk has a handle.
		 */
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
	/* Another X11 application has asked for clipboard data */
	struct xs_info **data = vdata;
	struct xs_info *xsi = *data;
	char *s;

	if (!xsi)
		return;
	if (cb == xsi->primary.cb)
		/* If there is an active selection, now if the time for
		 * the content to be copied.
		 */
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
	/* Some other X11 application wants us to release ownership
	 * of the clipboard.
	 */
	if (data == xsi->primary.data) {
		/* This means some other application now has a "selection",
		 * so we claim it on their behalf.
		 */
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
	/* Some edlib pane own the selection, so we renounce any ownership
	 * by any X11 application.
	 */
	call("selection:discard", ci->home);
	return Efallthrough;
}

DEF_CMD(xs_copy_get)
{
	struct xs_info *xsi = ci->home->data;
	int num = ci->num;

	if (xsi->clipboard.data == NULL) {
		if (num == 0) {
			/* Return CLIPBOARD if it exists */
			gchar *s = NULL;

			if (gtk_clipboard_wait_is_text_available(
				    xsi->clipboard.cb))
				s = gtk_clipboard_wait_for_text(
					xsi->clipboard.cb);
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
		return Efallthrough;
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
		return Efallthrough;

	if (xsi->primary.data || xsi->primary.saved)
		/* We own the primary, so nothing to do */
		return 1;

	if (!xsi->clipboard.data && !xsi->clipboard.saved) {
		/* get the clipboard first - to make sure it is available
		 * as second saved text.
		 */
		s = NULL;
		if (gtk_clipboard_wait_is_text_available(
			    xsi->clipboard.cb))
			s = gtk_clipboard_wait_for_text(xsi->clipboard.cb);
		if (s && *s) {
			call("copy:save", ci->home->parent,
			     0, NULL, s);
			xsi->clipboard.saved = 1;
		}
		g_free(s);
	}
	s = NULL;
	if (gtk_clipboard_wait_is_text_available(xsi->primary.cb))
		s = gtk_clipboard_wait_for_text(xsi->primary.cb);
	if (s && *s) {
		call("copy:save", ci->home->parent,
		     0, NULL, s);
		xsi->primary.saved = 1;
	}
	g_free(s);

	return Efallthrough;
}

DEF_CMD(xs_close)
{
	struct xs_info *xsi = ci->home->data;

	if (xsi->primary.data)
		gtk_clipboard_clear(xsi->primary.cb);
	if (xsi->clipboard.data)
		gtk_clipboard_clear(xsi->clipboard.cb);
	free(xsi->primary.data);
	free(xsi->clipboard.data);
	gtk_target_table_free(xsi->text_targets, xsi->n_text_targets);
	gdk_display_close(xsi->display);

	return 1;
}

DEF_CMD(xs_clone)
{
	struct pane *p;

	p = call_ret(pane, "attach-x11selection", ci->focus);
	pane_clone_children(ci->home, p);
	return 1;
}

static struct map *xs_map;
DEF_LOOKUP_CMD(xs_handle, xs_map);

DEF_CMD(xs_attach)
{
	char *d;
	struct xs_info *xsi;
	struct pane *p;
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

	xsi->display = dis;
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

	p = pane_register(ci->focus, 0, &xs_handle.c, xsi);
	if (!p)
		return Efail;
	xsi->self = p;
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
		key_add(xs_map, "Clone", &xs_clone);
		key_add(xs_map, "Close", &xs_close);
		key_add(xs_map, "Free", &edlib_do_free);
	}

	call_comm("global-set-command", ed, &xs_attach,
		  0, NULL, "attach-x11selection");
}
