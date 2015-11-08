/*
 * Copyright Neil Brown <neil@brown.name> 2015
 * May be distrubuted under terms of GPLv2 - see file:COPYING
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

static int add_ent(struct directory *dr, struct dirent *de)
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
	before = list_first_entry(&dr->ents, struct dir_ent, lst);
	while (&before->lst != &dr->ents &&
	       strcmp(dre->name, before->name) > 0)
		before = list_next_entry(before, lst);
	list_add_tail(&dre->lst, &before->lst);
	return 1;
}

static void load_dir(struct directory *dr, int fd)
{
	DIR *dir;
	struct dirent de, *res;

	dir = fdopendir(dup(fd));
	if (!dir)
		return;
	while (readdir_r(dir, &de, &res) == 0 && res)
		add_ent(dr, res);
	closedir(dir);
}

static struct doc *dir_new(struct doctype *dt)
{
	struct directory *dr = malloc(sizeof(*dr));
	doc_init(&dr->doc);
	dr->doc.map = doc_map;
	dr->doc.default_render = "dir";
	dr->doc.ops = &dir_ops;
	INIT_LIST_HEAD(&dr->ents);
	dr->fname = NULL;
	return &dr->doc;
}

static void dir_replace(struct point *pos, struct mark *end,
			 char *str, bool *first)
{
}

static int dir_load_file(struct doc *d, struct point *pos,
			 int fd, char *name)
{
	struct directory *dr = container_of(d, struct directory, doc);

	load_dir(dr, fd);
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
	return 1;
}

static int dir_same_file(struct doc *d, int fd, struct stat *stb)
{
	struct directory *dr = container_of(d, struct directory, doc);

	if (!dr->fname)
		return 0;
	return (dr->stat.st_ino == stb->st_ino &&
		dr->stat.st_dev == stb->st_dev);
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
			return "    Mtime       Owner  File Name";
		if (strcmp(attr, "line-format") == 0)
			return "  %mtime:11 %owner:-8 %+name";
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

static struct doctype dirtype = {
	.name = "dir",
	.new = dir_new,
};

static int doc_dir_open(struct command *c, struct cmd_info *ci)
{
	struct pane *p = ci->home;
	struct point *pt = p->point;
	struct doc *d = pt->doc;
	struct directory *dr = container_of(d, struct directory, doc);
	struct dir_ent *de = pt->m.ref.d;
	struct pane *par = p->parent;
	int fd;
	char *fname = NULL;

	/* close this pane, open the given file. */
	if (de == NULL)
		return 0;
	asprintf(&fname, "%s/%s", dr->fname, de->name);
	fd = open(fname, O_RDONLY);
	pane_close(p);
	if (fd >= 0) {
		p = doc_open(par, fd, fname, NULL);
		close(fd);
	} else
		p = doc_from_text(par, fname, "File not found\n");
	free(fname);
	pane_focus(p);
	return 1;
}
DEF_CMD(comm_open, doc_dir_open);

void edlib_init(struct editor *ed)
{
	doc_register_type(ed, &dirtype);

	doc_map = key_alloc();
	key_add(doc_map, "Open", &comm_open);
}
