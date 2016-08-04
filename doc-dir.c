/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
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
#include <pwd.h>
#include <grp.h>
#include <time.h>

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

static struct map *doc_map;

static int add_ent(struct list_head *lst safe, struct dirent *de safe)
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

static void load_dir(struct list_head *lst safe, int fd)
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

DEF_LOOKUP_CMD_DFLT(doc_handle, doc_map, doc_default_cmd);

DEF_CMD(dir_new)
{
	struct directory *dr = malloc(sizeof(*dr));
	struct pane *p;

	doc_init(&dr->doc);
	INIT_LIST_HEAD(&dr->ents);
	dr->fname = NULL;
	p = pane_register(ci->home, 0, &doc_handle.c, &dr->doc, NULL);
	dr->doc.home = p;
	if (p)
		return comm_call(ci->comm2, "callback:doc", p, 0, NULL, NULL, 0);
	return -1;
}

DEF_CMD(dir_new2)
{
	if (ci->extra != S_IFDIR)
		return 0;
	return dir_new_func(ci);
}

DEF_CMD(dir_load_file)
{
	struct doc *d = ci->home->data;
	int fd = ci->extra;
	char *name = ci->str;
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
	prev = m = doc_first_mark_all(&dr->doc);
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
			     m2 = doc_next_mark_all(m2))
				m2->ref.d = de1;
			attr_free(&de->attrs);
			free(de->name);
			list_del(&de->lst);
			free(de);
			if (m && donotify) {
				doc_notify_change(&dr->doc, prev, NULL);
				doc_notify_change(&dr->doc, m, NULL);
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
				doc_notify_change(&dr->doc, prev, NULL);
				doc_notify_change(&dr->doc, m, NULL);
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
			m = doc_next_mark_all(m);
		}
	}
	if (!donotify) {
		m = doc_first_mark_all(&dr->doc);
		if (m)
			doc_notify_change(&dr->doc, m, NULL);
	}

	if (name) {
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
		call5("doc:set-name", ci->home, 0, NULL, dname, 0);
		if (l > 1)
			strcat(dr->fname, "/");
	}
	if (doclose)
		close(fd);
	return 1;
}

DEF_CMD(dir_same_file)
{
	struct doc *d = ci->home->data;
	int fd = ci->extra;
	struct stat stb;
	struct directory *dr = container_of(d, struct directory, doc);

	if (!dr->fname)
		return 0;
	if (fstat(fd, &stb) != 0)
		return 0;
	if (! (dr->stat.st_ino == stb.st_ino &&
	       dr->stat.st_dev == stb.st_dev))
		return 0;
	/* Let's reload it now */
	comm_call(&dir_load_file, "doc:load-file", ci->focus, 0, NULL, NULL, fd);
	return 1;
}

DEF_CMD(dir_step)
{
	struct doc *doc = ci->home->data;
	struct mark *m = ci->mark;
	struct mark *m2, *target = m;
	bool forward = ci->numeric;
	bool move = ci->extra;
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
		if (move)
			for (m2 = doc_next_mark_all(m);
			     m2 && (m2->ref.d == d || m2->ref.d == m->ref.d);
			     m2 = doc_next_mark_all(m2))
				target = m2;
	} else {
		if (d == list_first_entry(&dr->ents, struct dir_ent, lst))
			d = NULL;
		else if (d == NULL)
			d = list_last_entry(&dr->ents, struct dir_ent, lst);
		else
			d = list_prev_entry(d, lst);
		if (d)
			ret = d->ch;
		else {
			ret = WEOF;
			d = m->ref.d;
		}
		if (move)
			for (m2 = doc_prev_mark_all(m);
			     m2 && (m2->ref.d == d || m2->ref.d == m->ref.d);
			     m2 = doc_prev_mark_all(m2))
				target = m2;
	}
	if (move) {
		mark_to_mark(m, target);
		m->ref.d = d;
	}
	/* return value must be +ve, so use high bits to ensure this. */
	return (ret & 0xFFFFF) | 0x100000;
}

DEF_CMD(dir_set_ref)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct mark *m = ci->mark;

	if (list_empty(&dr->ents) || ci->numeric != 1)
		m->ref.d = NULL;
	else
		m->ref.d = list_first_entry(&dr->ents, struct dir_ent, lst);
	m->ref.ignore = 0;
	m->rpos = 0;
	return 1;
}

DEF_CMD(dir_mark_same)
{
	return ci->mark->ref.d == ci->mark2->ref.d ? 1 : 2;
}

static void get_stat(struct directory *dr safe, struct dir_ent *de safe)
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

static char *fmt_num(struct dir_ent *de safe, long num)
{
	sprintf(de->nbuf, "%ld", num);
	return de->nbuf;
}

static char *save_str(struct dir_ent *de safe, char *str safe)
{
	strncpy(de->nbuf, str, sizeof(de->nbuf));
	de->nbuf[sizeof(de->nbuf)-1] = 0;
	return de->nbuf;
}

static char *fmt_date(struct dir_ent *de safe, time_t t)
{
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(de->nbuf, sizeof(de->nbuf),
		 "%b %d %H:%M", &tm);
	return de->nbuf;
}

static char *__dir_get_attr(struct doc *d safe, struct mark *m safe,
			    bool forward, char *attr safe)

{
	struct dir_ent *de;
	struct directory *dr = container_of(d, struct directory, doc);

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
	} else if (strcmp(attr, "mdate") == 0) {
		get_stat(dr, de);
		return fmt_date(de, de->st.st_mtime);
	} else if (strcmp(attr, "atime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_atime);
	} else if (strcmp(attr, "adate") == 0) {
		get_stat(dr, de);
		return fmt_date(de, de->st.st_atime);
	} else if (strcmp(attr, "ctime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_ctime);
	} else if (strcmp(attr, "cdate") == 0) {
		get_stat(dr, de);
		return fmt_date(de, de->st.st_ctime);
	} else if (strcmp(attr, "uid") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_uid);
	} else if (strcmp(attr, "gid") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_gid);
	} else if (strcmp(attr, "user") == 0) {
		struct passwd *pw;
		get_stat(dr, de);
		pw = getpwuid(de->st.st_uid);
		if (pw && pw->pw_name)
			return save_str(de, pw->pw_name);
		else
			return fmt_num(de, de->st.st_uid);
	} else if (strcmp(attr, "group") == 0) {
		struct group *gr;
		get_stat(dr, de);
		gr = getgrgid(de->st.st_gid);
		if (gr && gr->gr_name)
			return save_str(de, gr->gr_name);
		else
			return fmt_num(de, de->st.st_gid);
	} else if (strcmp(attr, "mode") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_mode & 0777);
	} else if (strcmp(attr, "perms") == 0) {
		char *c;
		int mode;
		int i;
		get_stat(dr, de);
		c = de->nbuf;
		mode = de->st.st_mode;
		switch (mode & S_IFMT) {
		case S_IFREG: *c ++ = '-'; break;
		case S_IFDIR: *c ++ = 'd'; break;
		case S_IFBLK: *c ++ = 'b'; break;
		case S_IFCHR: *c ++ = 'c'; break;
		case S_IFSOCK:*c ++ = 's'; break;
		case S_IFLNK: *c ++ = 'l'; break;
		default:      *c ++ = '?'; break;
		}
		for (i = 0; i < 3; i++) {
			*c ++ = (mode & 0400) ? 'r':'-';
			*c ++ = (mode & 0200) ? 'w':'-';
			*c ++ = (mode & 0100) ? 'x':'-';
			mode = mode << 3;
		}
		*c = 0;
		return de->nbuf;
	} else if (strcmp(attr, "suffix") == 0) {
		get_stat(dr, de);
		if ((de->st.st_mode & S_IFMT) == S_IFDIR)
			return "/";
		else
			return "";
	} else
		return attr_find(de->attrs, attr);
}

DEF_CMD(dir_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->numeric != 0;
	char *attr = ci->str;
	char *val;

	if (!m)
		return -1;
	val = __dir_get_attr(d, m, forward, attr);

	if (!val)
		return 0;
	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val, 0);
	return 1;
}

DEF_CMD(dir_get_attr)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	char *attr = ci->str;
	char *val;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
		val = "<bold,fg:blue,underline>  Perms       Mtime       Owner      Group      File Name</>";
	else if (strcmp(attr, "render-default") == 0)
		val = "format";
	else if (strcmp(attr, "doc-type") == 0)
		val = "dir";
	else if (strcmp(attr, "line-format") == 0)
		val = " <fg:red>%perms</> %mdate:13 %user:10 %group:10 <fg:blue>%+name</>";
	else if (strcmp(attr, "filename") == 0)
		val = dr->fname;
	else
		return 0;
	comm_call(ci->comm2, "callback:get_attr", ci->focus,
		  0, NULL, val, 0);
	return 1;
}

DEF_CMD(dir_destroy)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);

	while (!list_empty(&dr->ents)) {
		struct dir_ent *de = list_entry(dr->ents.next, struct dir_ent, lst);

		attr_free(&de->attrs);
		free(de->name);
		list_del(&de->lst);
		free(de);
	}
	doc_free(d);
	free(dr);
	return 1;
}

DEF_CMD(dir_open)
{
	struct pane *p = ci->home;
	struct doc *d = p->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct dir_ent *de = ci->mark->ref.d;
	struct pane *par;
	int fd;
	char *fname = NULL;

	/* close this pane, open the given file. */
	if (de == NULL)
		return 0;

	asprintf(&fname, "%s/%s", dr->fname, de->name);
	fd = open(fname, O_RDONLY);
	if (strcmp(ci->key, "Chr-o") == 0)
		par = call_pane("OtherPane", ci->focus, 0, NULL, 0);
	else
		par = call_pane("ThisPane", ci->focus, 0, NULL, 0);

	if (fd >= 0) {
		p = call_pane7("doc:open", ci->focus, fd, NULL, 0, fname, NULL);
		close(fd);
	} else
		p = call_pane7("doc:from-text", ci->focus, 0, NULL, 0, fname,
			       "File not found\n");
	free(fname);
	if (par && p) {
		p = doc_attach_view(par, p, NULL);
		pane_focus(p);
	}
	return 1;
}

DEF_CMD(dir_open_alt)
{
	struct pane *p = ci->home;
	struct doc *d = p->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct dir_ent *de = ci->mark->ref.d;
	struct pane *par = p->parent;
	int fd;
	char *fname = NULL;
	char *renderer = NULL;
	char buf[100];

	/* close this pane, open the given file. */
	if (de == NULL)
		return 0;
	snprintf(buf, sizeof(buf), "render-%s", ci->key);
	asprintf(&fname, "%s/%s", dr->fname, de->name);
	fd = open(fname, O_RDONLY);

	if (fd >= 0) {
		struct pane *new = call_pane7("doc:open", p, fd, NULL, 0, fname, NULL);
		if (new) {
			renderer = pane_attr_get(new, buf);
			if (renderer) {
				par = call_pane("ThisPane", ci->focus, 0, NULL, 0);
				if (!par)
					return -1;

				p = doc_attach_view(par, new, renderer);
			}
		}
		close(fd);
	} else {
		struct pane *doc = call_pane7("doc:from-text", par, 0, NULL, 0,
					      fname, "File not found\n");
		par = call_pane("ThisPane", ci->focus, 0, NULL, 0);
		if (!par)
			return -1;
		p = doc_attach_view(par, doc, NULL);
	}
	free(fname);
	pane_focus(p);
	return 1;
}

DEF_CMD(dir_reread)
{
	return comm_call(&dir_load_file, "doc:load-file", ci->focus, 0, NULL, NULL, -1);
}

DEF_CMD(dir_close)
{
	return call3("doc:destroy", ci->home, 0, NULL);
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, 0, NULL, "attach-doc-dir",
		  0, &dir_new);
	call_comm("global-set-command", ed, 0, NULL, "open-doc-dir",
		  0, &dir_new2);

	doc_map = key_alloc();
	key_add(doc_map, "Chr-f", &dir_open);
	key_add(doc_map, "Return", &dir_open);
	key_add_range(doc_map, "Chr-A", "Chr-Z", &dir_open_alt);
	key_add(doc_map, "Chr-o", &dir_open);
	key_add(doc_map, "Chr-g", &dir_reread);
	key_add(doc_map, "Chr-q", &dir_close);

	key_add(doc_map, "doc:load-file", &dir_load_file);
	key_add(doc_map, "doc:same-file", &dir_same_file);
	key_add(doc_map, "Close", &dir_destroy);
	key_add(doc_map, "doc:set-ref", &dir_set_ref);
	key_add(doc_map, "doc:get-attr", &dir_doc_get_attr);
	key_add(doc_map, "get-attr", &dir_get_attr);
	key_add(doc_map, "doc:mark-same", &dir_mark_same);
	key_add(doc_map, "doc:step", &dir_step);
}
