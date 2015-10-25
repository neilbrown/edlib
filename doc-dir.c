/*
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

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdio.h>

#define PRIVATE_DOC_REF
struct doc_ref {
	struct dir_ent	*d;
	int ignore;
};

#include "core.h"
#include "attr.h"

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
};
static struct doc_operations dir_ops;

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
	else if (strcmp(de->d_name, ".") == 0)
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
	dr->doc.ops = &dir_ops;
	INIT_LIST_HEAD(&dr->ents);
	return &dr->doc;
}

static void dir_replace(struct point *pos, struct mark *end,
			 char *str, bool *first)
{
}

static int dir_load_file(struct point *pos, int fd)
{
	struct doc *d = pos->doc;
	struct directory *dr = container_of(d, struct directory, doc);
	load_dir(dr, fd);
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

static void get_stat(struct dir_ent *de)
{
	if (de->st.st_mode)
		return;
	if (lstat(de->name, &de->st) != 0) {
		de->st.st_mode = 0xffff;
		de->ch = '?';
	}
}

static char *fmt_num(struct dir_ent *de, long num)
{
	sprintf(de->nbuf, "%ld", num);
	return de->nbuf;
}

static char *dir_get_attr(struct doc *d, struct mark *m,
			  bool forward, char *attr)
{
	struct dir_ent *de = m->ref.d;
	struct directory *dr = container_of(d, struct directory, doc);

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
		get_stat(de);
		return fmt_num(de, de->st.st_mtime);
	} else if (strcmp(attr, "atime") == 0) {
		get_stat(de);
		return fmt_num(de, de->st.st_atime);
	} else if (strcmp(attr, "ctime") == 0) {
		get_stat(de);
		return fmt_num(de, de->st.st_ctime);
	} else if (strcmp(attr, "owner") == 0) {
		get_stat(de);
		return fmt_num(de, de->st.st_uid);
	} else if (strcmp(attr, "group") == 0) {
		get_stat(de);
		return fmt_num(de, de->st.st_gid);
	} else if (strcmp(attr, "modes") == 0) {
		get_stat(de);
		return fmt_num(de, de->st.st_mode & 0777);
	} else
		return attr_get_str(de->attrs, attr, -1);
}

static int dir_set_attr(struct point *p, char *attr, char *val)
{
	return 0;
}

static struct doc_operations dir_ops = {
	.replace   = dir_replace,
	.load_file = dir_load_file,
	.reundo    = dir_reundo,
	.step      = dir_step,
	.get_str   = dir_getstr,
	.set_ref   = dir_setref,
	.same_ref  = dir_sameref,
	.get_attr  = dir_get_attr,
	.set_attr  = dir_set_attr,
};

static struct doctype dirtype = {
	.new = dir_new,
};

void doc_dir_register(struct editor *ed)
{
	doc_register_type(ed, "dir", &dirtype);
}
