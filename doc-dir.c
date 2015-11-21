/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * directory listing as a document.
 *
 * The 'text' of the document is a single char per director entry:
 * .  current directory
 * : parent directory
 * d  other directory
 * f  regular file
 * l  link
 * c  char-special
 * b  block-special
 * p  named-pipe
 * s  socket
 *
 * Each char has a set of attributes which give details
 * name   file name
 * size
 * mtime
 * atime
 * ctime
 * owner
 * group
 * modes
 * nlinks
 *
 */

#define _GNU_SOURCE /*  for asprintf */
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/stat.h>
#include <sys/fcntl.h>
#include <dirent.h>
#include <stdio.h>

#define PRIVATE_DOC_REF
struct doc_ref {
	struct dir_ent	*d;
	int ignore;
};

#include "core.h"

struct dir_ent {
	char			*name;
	wchar_t			ch;
	struct list_head	lst;
	struct attrset		*attrs;
	struct stat		st;
	char			nbuf[20];
};

struct directory {
	struct doc		doc;
	struct list_head	ents;

	struct stat		stat;
	char			*fname;
};
static struct doc_operations dir_ops;
static struct map *doc_map;

static int add_ent(struct list_head *lst, struct dirent *de)
{
	struct dir_ent *dre;
	struct dir_ent *before; /* insert before here */
	if (de->d_ino == 0)
		return 0;

	dre = malloc(sizeof(*dre));
	dre->name = strdup(de->d_name);
	dre->attrs = NULL;
	dre->st.st_mode = 0;
	if (strcmp(de->d_name, ".") == 0)
		dre->ch = '.';
	else if (strcmp(de->d_name, "..") == 0)
		dre->ch = ':';
	else switch (de->d_type) {
		case DT_BLK: dre->ch = 'b'; break;
		case DT_CHR: dre->ch = 'c'; break;
		case DT_DIR: dre->ch = 'd'; break;
		case DT_FIFO:dre->ch = 'p'; break;
		case DT_LNK: dre->ch = 'l'; break;
		case DT_REG: dre->ch = 'f'; break;
		case DT_SOCK:dre->ch = 's'; break;
		default:
		case DT_UNKNOWN:dre->ch = '?'; break;
		}
	before = list_first_entry(lst, struct dir_ent, lst);
	while (&before->lst != lst &&
	       strcmp(dre->name, before->name) > 0)
		before = list_next_entry(before, lst);
	list_add_tail(&dre->lst, &before->lst);
	return 1;
}

static void load_dir(struct list_head *lst, int fd)
{
	DIR *dir;
	struct dirent de, *res;

	dir = fdopendir(dup(fd));
	if (!dir)
		return;
	while (readdir_r(dir, &de, &res) == 0 && res)
		add_ent(lst, res);
	closedir(dir);
}

DEF_CMD(comm_new)
{
	struct directory *dr = malloc(sizeof(*dr));
	doc_init(&dr->doc);
	dr->doc.map = doc_map;
	dr->doc.default_render = "format";
	dr->doc.ops = &dir_ops;
	INIT_LIST_HEAD(&dr->ents);
	dr->fname = NULL;
	point_new(&dr->doc, ci->pointp);
	return 1;
}

static void dir_replace(struct point *pos, struct mark *end,
			 char *str, bool *first)
{
}

static int dir_load_file(struct doc *d, struct point *pos,
			 int fd, char *name)
{
	struct directory *dr = container_of(d, struct directory, doc);
	struct list_head new;
	struct dir_ent *de1, *de2;
	struct mark *m, *prev;
	int doclose = 0;
	int donotify = 1;

	if (fd < 0) {
		if (!dr->fname)
			return -1;
		fd = open(dr->fname, O_RDONLY|O_DIRECTORY);
		if (fd < 0)
			return -1;
		doclose = 1;
	}

	INIT_LIST_HEAD(&new);
	load_dir(&new, fd);
	de1 = list_first_entry_or_null(&dr->ents, struct dir_ent, lst);
	de2 = list_first_entry_or_null(&new, struct dir_ent, lst);
	if (!de1)
		/* Nothing already in dir, so only notify at the end */
		donotify = 1;
	prev = m = doc_first_mark_all(d);
	/* 'm' is always at-or-after the earliest of de1 */
	while (de1 || de2) {
		if (de1 &&
		    (de2 == NULL || strcmp(de1->name, de2->name) < 0)) {
			/* de1 doesn't exist in new: need to delete it. */
			struct mark *m2;
			struct dir_ent *de = de1;
			if (de1 == list_last_entry(&dr->ents, struct dir_ent, lst))
				de1 = NULL;
			else
				de1 = list_next_entry(de1, lst);
			for (m2 = m; m2 && m2->ref.d == de;
			     m2 = doc_next_mark_all(d, m2))
				m2->ref.d = de1;
			attr_free(&de->attrs);
			free(de->name);
			list_del(&de->lst);
			free(de);
			if (m && donotify) {
				doc_notify_change(d, prev);
				doc_notify_change(d, m);
			}
		} else if (de2 &&
			   (de1 == NULL || strcmp(de2->name, de1->name) < 0)) {
			/* de2 doesn't already exist, so add it before de1 */
			list_del(&de2->lst);
			if (de1)
				list_add_tail(&de2->lst, &de1->lst);
			else
				list_add_tail(&de2->lst, &dr->ents);
			if (m && donotify) {
				doc_notify_change(d, prev);
				doc_notify_change(d, m);
			}
		} else {
			/* de1 and de2 are the same.  Just step over de1 and
			 * delete de2
			 */
			if (de1 == list_last_entry(&dr->ents, struct dir_ent, lst))
				de1 = NULL;
			else
				de1 = list_next_entry(de1, lst);
			list_del(&de2->lst);
			attr_free(&de2->attrs);
			free(de2->name);
			free(de2);
		}
		de2 = list_first_entry_or_null(&new, struct dir_ent, lst);
		while (m && m->ref.d && de1 && strcmp(m->ref.d->name, de1->name) < 0) {
			prev = m;
			m = doc_next_mark_all(d, m);
		}
	}
	if (!donotify) {
		m = doc_first_mark_all(d);
		if (m)
			doc_notify_change(d, m);
	}

	if (name && !pos) {
		char *dname;
		int l = strlen(name);

		fstat(fd, &dr->stat);
		free(dr->fname);
		dr->fname = malloc(l+2);
		strcpy(dr->fname, name);
		if (l > 1 && dr->fname[l-1] == '/')
			dr->fname[l-1] = '\0';
		dname = strrchr(dr->fname, '/');
		if (dname && dname[1])
			dname += 1;
		else
			dname = name;
		doc_set_name(d, dname);
		if (l > 1)
			strcat(dr->fname, "/");
	}
	if (doclose)
		close(fd);
	return 1;
}

static int dir_same_file(struct doc *d, int fd, struct stat *stb)
{
	struct directory *dr = container_of(d, struct directory, doc);

	if (!dr->fname)
		return 0;
	if (! (dr->stat.st_ino == stb->st_ino &&
	       dr->stat.st_dev == stb->st_dev))
		return 0;
	/* Let's reload it now */
	dir_load_file(d, NULL, fd, NULL);
	return 1;
}

static int dir_reundo(struct point *p, bool redo)
{
	return 0;
}

static wint_t dir_step(struct doc *doc, struct mark *m, bool forward, bool move)
{
	struct directory *dr = container_of(doc, struct directory, doc);
	struct dir_ent *d = m->ref.d;
	wint_t ret;

	if (forward) {
		if (d == NULL)
			ret = WEOF;
		else {
			ret = d->ch;
			if (d == list_last_entry(&dr->ents, struct dir_ent, lst))
				d = NULL;
			else
				d = list_next_entry(d, lst);
		}
	} else {
		if (d == list_first_entry(&dr->ents, struct dir_ent, lst))
			d = NULL;
		else if (d == NULL)
			d = list_last_entry(&dr->ents, struct dir_ent, lst);
		else
			d = list_prev_entry(d, lst);
		if (d)
			ret = d->ch;
		else
			ret = WEOF;
	}
	if (move && ret != WEOF)
		m->ref.d = d;
	return ret;
}

static char *dir_getstr(struct doc *d, struct mark *from, struct mark *to)
{
	return NULL;
}

static void dir_setref(struct doc *doc, struct mark *m, bool start)
{
	struct directory *dr = container_of(doc, struct directory, doc);

	if (list_empty(&dr->ents) || !start)
		m->ref.d = NULL;
	else
		m->ref.d = list_first_entry(&dr->ents, struct dir_ent, lst);
	m->ref.ignore = 0;
}

static int dir_sameref(struct doc *d, struct mark *a, struct mark *b)
{
	return a->ref.d == b->ref.d;
}

static void get_stat(struct directory *dr, struct dir_ent *de)
{
	int dfd;
	if (de->st.st_mode)
		return;
	dfd = open(dr->fname, O_RDONLY);
	if (!dfd)
		return;
	if (fstatat(dfd, de->name, &de->st, AT_SYMLINK_NOFOLLOW) != 0) {
		de->st.st_mode = 0xffff;
		de->ch = '?';
	}
	close(dfd);
}

static char *fmt_num(struct dir_ent *de, long num)
{
	sprintf(de->nbuf, "%ld", num);
	return de->nbuf;
}

static char *dir_get_attr(struct doc *d, struct mark *m,
			  bool forward, char *attr)
{
	struct dir_ent *de;
	struct directory *dr = container_of(d, struct directory, doc);

	if (!m) {
		char *a = attr_get_str(d->attrs, attr, -1);
		if (a)
			return a;
		if (strcmp(attr, "heading") == 0)
			return "<bold,fg:blue,underline>     Mtime       Owner  File Name</>";
		if (strcmp(attr, "line-format") == 0)
			return " <fg:red>%c</> %mtime:11 %owner:-8 <fg:blue>%+name</>";
		if (strcmp(attr, "filename") == 0)
			return dr->fname;
		return NULL;
	}
	de = m->ref.d;
	if (!forward) {
		if (!de)
			de = list_last_entry(&dr->ents, struct dir_ent, lst);
		else
		de = list_prev_entry(de, lst);
		if (&de->lst == &dr->ents)
			return NULL;
	}
	if (!de)
		return NULL;
	if (strcmp(attr, "name") == 0) {
		return de->name;
	} else if (strcmp(attr, "mtime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_mtime);
	} else if (strcmp(attr, "atime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_atime);
	} else if (strcmp(attr, "ctime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_ctime);
	} else if (strcmp(attr, "owner") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_uid);
	} else if (strcmp(attr, "group") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_gid);
	} else if (strcmp(attr, "modes") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_mode & 0777);
	} else
		return attr_get_str(de->attrs, attr, -1);
}

static int dir_set_attr(struct point *p, char *attr, char *val)
{
	return 0;
}

static void dir_destroy(struct doc *d)
{
	struct directory *dr = container_of(d, struct directory, doc);

	while (!list_empty(&dr->ents)) {
		struct dir_ent *de = list_entry(dr->ents.next, struct dir_ent, lst);

		attr_free(&de->attrs);
		free(de->name);
		list_del(&de->lst);
		free(de);
	}
	free(d);
}


static struct doc_operations dir_ops = {
	.replace   = dir_replace,
	.load_file = dir_load_file,
	.same_file = dir_same_file,
	.reundo    = dir_reundo,
	.step      = dir_step,
	.get_str   = dir_getstr,
	.set_ref   = dir_setref,
	.same_ref  = dir_sameref,
	.get_attr  = dir_get_attr,
	.set_attr  = dir_set_attr,
	.destroy   = dir_destroy,
};

DEF_CMD(dir_open)
{
	struct pane *p = ci->home;
	struct point *pt = p->point;
	struct doc *d = pt->doc;
	struct directory *dr = container_of(d, struct directory, doc);
	struct dir_ent *de = pt->m.ref.d;
	struct pane *par = p->parent;
	int fd;
	char *fname = NULL;
	char *renderer = NULL;

	/* close this pane, open the given file. */
	if (de == NULL)
		return 0;
	if (strcmp(ci->key, "Chr-h") == 0)
		renderer = "hex";
	asprintf(&fname, "%s/%s", dr->fname, de->name);
	fd = open(fname, O_RDONLY);
	if (strcmp(ci->key, "Chr-o") == 0) {
		struct cmd_info ci2 = {0};
		ci2.key = "OtherPane";
		ci2.focus = ci->focus;
		if (key_handle_focus(&ci2)) {
			par = ci2.focus;
			p = pane_child(par);
		}
	}
	if (p)
		pane_close(p);
	if (fd >= 0) {
		p = doc_open(par, fd, fname, renderer);
		close(fd);
	} else
		p = doc_from_text(par, fname, "File not found\n");
	free(fname);
	pane_focus(p);
	return 1;
}

DEF_CMD(dir_reread)
{
	struct doc *d = (*ci->pointp)->doc;
	d->ops->load_file(d, NULL, -1, NULL);
	return 1;
}

DEF_CMD(dir_close)
{
	struct doc *d = (*ci->pointp)->doc;

	doc_close_views(d);
	doc_destroy(d);
	return 1;
}


void edlib_init(struct editor *ed)
{
	key_add(ed->commands, "doc-dir", &comm_new);

	doc_map = key_alloc();
	key_add(doc_map, "Chr-f", &dir_open);
	key_add(doc_map, "Return", &dir_open);
	key_add(doc_map, "Chr-h", &dir_open);
	key_add(doc_map, "Chr-o", &dir_open);
	key_add(doc_map, "Chr-g", &dir_reread);
	key_add(doc_map, "Chr-q", &dir_close);
}
