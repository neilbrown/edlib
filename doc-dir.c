/*
 * Copyright Neil Brown ©2015-2020 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * directory listing as a document.
 *
 * The 'text' of the document is a single '\n' char per director entry:
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
 * type:
 *  .  current directory
 *  : parent directory
 *  d  other directory
 *  f  regular file
 *  l  link
 *  c  char-special
 *  b  block-special
 *  p  named-pipe
 *  s  socket
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
	char			*name safe;
	wchar_t			ch;
	struct list_head	lst;
	struct attrset		*attrs;
	struct stat		st;
	char			nbuf[20];
};

struct directory {
	struct doc		doc;
	struct list_head	ents;
	struct pane		*rendering;

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
	struct dirent *res;

	dir = fdopendir(dup(fd));
	if (!dir)
		return;
	while ((res = readdir(dir)) != NULL)
		add_ent(lst, res);
	closedir(dir);
}

DEF_LOOKUP_CMD(doc_handle, doc_map);

DEF_CMD(dir_new)
{
	struct directory *dr;
	struct pane *p;

	alloc(dr, pane);
	INIT_LIST_HEAD(&dr->ents);
	dr->fname = NULL;
	doc_register(ci->home, 0, &doc_handle.c, dr);
	p = call_ret(pane, "attach-render-format", dr->doc.home, 1);
	if (p)
		p = call_ret(pane, "attach-doc-rendering", p);
	if (p)
		pane_add_notify(dr->doc.home, p, "Notify:Close");
	dr->rendering = p;
	return comm_call(ci->comm2, "callback:doc", dr->doc.home);
}

DEF_CMD(dir_new2)
{
	if (ci->num2 != S_IFDIR)
		return 0;
	return dir_new_func(ci);
}

DEF_CMD(dir_load_file)
{
	struct doc *d = ci->home->data;
	int fd = ci->num2;
	const char *name = ci->str;
	struct directory *dr = container_of(d, struct directory, doc);
	struct list_head new;
	struct dir_ent *de1, *de2;
	struct mark *prev, *m;
	int doclose = 0;

	prev = NULL;
	m = vmark_new(ci->home, MARK_UNGROUPED, NULL);
	if (!m)
		return Efail;
	if (fd < 0) {
		if (!dr->fname)
			return Efail;
		fd = open(dr->fname, O_RDONLY|O_DIRECTORY);
		if (fd < 0)
			return Efail;
		doclose = 1;
	}

	INIT_LIST_HEAD(&new);
	load_dir(&new, fd);
	de2 = list_first_entry_or_null(&new, struct dir_ent, lst);
	while (m->ref.d || de2) {
		de1 = m->ref.d;
		if (de1 &&
		    (de2 == NULL || strcmp(de1->name, de2->name) < 0)) {
			/* de1 doesn't exist in new: need to delete it. */
			struct mark *m2;
			struct dir_ent *de = de1;
			if (de1 == list_last_entry(&dr->ents,
						   struct dir_ent, lst))
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
			if (!prev)
				prev = mark_dup(m);
		} else if (de2 &&
			   (de1 == NULL || strcmp(de2->name, de1->name) < 0)) {
			/* de2 doesn't already exist, so add it before de1 */
			list_del(&de2->lst);
			if (de1)
				list_add_tail(&de2->lst, &de1->lst);
			else
				list_add_tail(&de2->lst, &dr->ents);
			if (!prev) {
				prev = mark_dup(m);
				mark_prev_pane(ci->home, prev);
			}
		} else if (de1 && de2) {
			/* de1 and de2 are the same.  Just step over de1 and
			 * delete de2
			 */
			if (prev) {
				pane_notify("doc:replaced", ci->home,
					    0, prev, NULL,
					    0, m);
				mark_free(prev);
				prev = NULL;
			}
			mark_next_pane(ci->home, m);
			mark_make_first(m);

			list_del(&de2->lst);
			attr_free(&de2->attrs);
			free(de2->name);
			free(de2);
		}
		de2 = list_first_entry_or_null(&new, struct dir_ent, lst);
	}
	if (prev) {
		pane_notify("doc:replaced", ci->home, 0, prev, NULL,
			    0, m);
		mark_free(prev);
	}

	if (name) {
		const char *dname;
		int l = strlen(name);

		fstat(fd, &dr->stat);
		dr->fname = malloc(l+2);
		strcpy(dr->fname, name);
		if (l > 1 && dr->fname[l-1] == '/')
			dr->fname[l-1] = '\0';
		dname = strrchr(dr->fname, '/');
		if (dname && dname[1])
			dname += 1;
		else
			dname = name;
		call("doc:set-name", ci->home, 0, NULL, dname);
		if (l > 1)
			strcat(dr->fname, "/");
	}
	if (doclose)
		close(fd);
	return 1;
}

DEF_CMD(dir_revisited)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct stat st;

	if (ci->num <= 0)
		/* Being buried, not visited */
		return Efallthrough;

	if (stat(dr->fname, &st) == 0 &&
	    (st.st_ino != dr->stat.st_ino ||
	     st.st_dev != dr->stat.st_dev ||
	     st.st_mtime != dr->stat.st_mtime ||
	     st.st_mtim.tv_nsec != dr->stat.st_mtim.tv_nsec)) {
		call("doc:load-file", ci->home, 2, NULL, NULL, -1);
		call("Message", ci->focus, 0, NULL, "Directory Reloaded");
	}
	return Efallthrough;
}

DEF_CMD(dir_same_file)
{
	struct doc *d = ci->home->data;
	int fd = ci->num2;
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
	home_call(ci->home, "doc:load-file", ci->focus, 0, NULL, NULL, fd);
	return 1;
}

DEF_CMD(dir_step)
{
	struct doc *doc = ci->home->data;
	struct mark *m = ci->mark;
	bool forward = ci->num;
	bool move = ci->num2;
	struct directory *dr = container_of(doc, struct directory, doc);
	struct dir_ent *d;
	wint_t ret = '\n';

	if (!m)
		return Enoarg;
	d = m->ref.d;
	if (forward) {
		if (d == NULL)
			ret = WEOF;
		else {
			if (d == list_last_entry(&dr->ents,
						 struct dir_ent, lst))
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
		if (!d) {
			ret = WEOF;
			d = m->ref.d;
		}
	}
	if (move) {
		mark_step(m, forward);
		m->ref.d = d;
		mark_step(m, forward);
	}
	/* return value must be +ve, so use high bits to ensure this. */
	return CHAR_RET(ret);
}

DEF_CMD(dir_set_ref)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	if (list_empty(&dr->ents) || ci->num != 1)
		m->ref.d = NULL;
	else
		m->ref.d = list_first_entry(&dr->ents, struct dir_ent, lst);
	m->ref.ignore = 0;
	mark_to_end(d, m, ci->num != 1);
	return 1;
}

static void get_stat(struct directory *dr safe, struct dir_ent *de safe)
{
	int dfd;
	struct stat st;
	if (de->st.st_mode)
		return;
	dfd = open(dr->fname, O_RDONLY);
	if (!dfd)
		return;
	if (fstatat(dfd, de->name, &de->st, AT_SYMLINK_NOFOLLOW) != 0) {
		de->st.st_mode = 0xffff;
		de->ch = '?';
	} else if ((de->st.st_mode & S_IFMT) == S_IFLNK &&
		   fstatat(dfd, de->name, &st, 0) == 0 &&
		   (st.st_mode & S_IFMT) == S_IFDIR)
		de->ch = 'L';
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
	time_t now = time(NULL);
	char *faketime = getenv("EDLIB_FAKE_TIME");

	if (faketime)
		t = strtoul(faketime, NULL, 10);
	localtime_r(&t, &tm);
	if (t > now || t < now - 10*30*24*3600)
		strftime(de->nbuf, sizeof(de->nbuf),
			 "%b %d  %Y", &tm);
	else
		strftime(de->nbuf, sizeof(de->nbuf),
			 "%b %d %H:%M", &tm);
	return de->nbuf;
}

static const char *__dir_get_attr(struct doc *d safe, struct mark *m safe,
				  const char *attr safe)

{
	struct dir_ent *de;
	struct directory *dr = container_of(d, struct directory, doc);

	de = m->ref.d;
	if (!de)
		return NULL;
	if (strcmp(attr, "name") == 0) {
		return de->name;
	} else if (strcmp(attr, "type") == 0) {
		de->nbuf[0] = de->ch;
		de->nbuf[1] = 0;
		return de->nbuf;
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
		if (de->ch == 'l')
			get_stat(dr, de);
		if (strchr(".:dL", de->ch))
			return "/";
		return "";
	} else
		return attr_find(de->attrs, attr);
}

DEF_CMD(dir_doc_get_attr)
{
	struct doc *d = ci->home->data;
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val;

	if (!m || !attr)
		return Enoarg;
	val = __dir_get_attr(d, m, attr);

	if (!val)
		return 0;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(dir_get_attr)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	const char *attr = ci->str;
	const char *val;

	if (!attr)
		return Enoarg;

	if ((val = attr_find(d->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
		val = "<bold,fg:blue,underline>  Perms       Mtime       Owner      Group      File Name</>";
	else if (strcmp(attr, "render-default") == 0)
		val = "format";
	else if (strcmp(attr, "doc-type") == 0)
		val = "dir";
	else if (strcmp(attr, "line-format") == 0)
		val = " <fg:red>%.perms</> %.mdate:13 %.user:10 %.group:10 <fg:blue>%+name%.suffix</>";
	else if (strcmp(attr, "filename") == 0)
		val = dr->fname;
	else
		return 0;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(dir_destroy)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);

	while (!list_empty(&dr->ents)) {
		struct dir_ent *de = list_entry(dr->ents.next,
						struct dir_ent, lst);

		attr_free(&de->attrs);
		free(de->name);
		list_del(&de->lst);
		free(de);
	}
	if (dr->rendering)
		pane_close(dr->rendering);
	doc_free(d);
	return 1;
}

static int dir_open(struct pane *home safe, struct pane *focus safe,
		    struct mark *m, char cmd)
{
	struct doc *d = home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct dir_ent *de;
	struct pane *par, *p;
	int fd;
	char *fname = NULL;

	if (!m)
		return Enoarg;
	de = m->ref.d;
	/* close this pane, open the given file. */
	if (de == NULL)
		return 0;

	asprintf(&fname, "%s/%s", dr->fname, de->name);
	fd = open(fname, O_RDONLY);

	if (fd >= 0) {
		p = call_ret(pane, "doc:open", focus, fd, NULL, fname);
		close(fd);
	} else
		p = call_ret(pane, "doc:from-text", focus, 0, NULL, fname,
			     0, NULL, "File not found\n");
	free(fname);
	if (!p)
		return Efail;
	if (cmd == 'o') {
		par = home_call_ret(pane, focus, "DocPane", p);
		if (!par)
			par = call_ret(pane, "OtherPane", focus);
	} else
		par = call_ret(pane, "ThisPane", focus);
	if (par) {
		p = home_call_ret(pane, p, "doc:attach-view", par);
		pane_focus(p);
	}
	return 1;
}

static int dir_open_alt(struct pane *home safe, struct pane *focus safe,
			struct mark *m, char cmd)
{
	struct doc *d = home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	struct dir_ent *de;
	struct pane *p = home,  *par = home->parent;
	int fd;
	char *fname = NULL;
	char buf[100];

	if (!m)
		return Enoarg;
	de = m->ref.d;
	/* close this pane, open the given file. */
	if (de == NULL)
		return Efail;
	asprintf(&fname, "%s/%s", dr->fname, de->name);
	fd = open(fname, O_RDONLY);

	if (fd >= 0) {
		struct pane *new = call_ret(pane, "doc:open", home,
					    fd, NULL, fname);
		if (new) {
			snprintf(buf, sizeof(buf), "cmd-%c", cmd);
			par = call_ret(pane, "ThisPane", focus);
			if (!par)
				return Efail;

			p = home_call_ret(pane, new, "doc:attach-view", par,
					  1, NULL, buf);
		}
		close(fd);
	} else {
		struct pane *doc = call_ret(pane, "doc:from-text", par,
					    0, NULL, fname,
					    0, NULL, "File not found\n");
		if (!doc)
			return Efail;
		par = call_ret(pane, "ThisPane", focus);
		if (!par)
			return Efail;
		p = home_call_ret(pane, doc, "doc:attach-view", par, 1);
	}
	free(fname);
	pane_focus(p);
	return 1;
}

DEF_CMD(dir_cmd)
{
	char cmd;

	if (!ci->str)
		return Enoarg;
	cmd = ci->str[0];
	switch(cmd) {
	case 'f':
	case '\n':
	case 'o':
		return dir_open(ci->home, ci->focus, ci->mark2, cmd);
	case 'g':
		return home_call(ci->home, "doc:load-file", ci->focus,
				 0, NULL, NULL, -1);
	case 'q':
		return call("doc:destroy", ci->home);
	default:
		if (cmd >= 'A' && cmd <= 'Z')
			return dir_open_alt(ci->home, ci->focus, ci->mark2, cmd);
		/* Doc has the final say on commands. */
		return 1;
	}
}

DEF_CMD(dir_attach)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);
	const char *type = ci->str ?: "default";
	struct pane *p;

	if (strcmp(type, "invisible") == 0 ||
	    ci->num2 == (int)(unsigned long)dir_attach_func)
		/* use default core-doc implementation */
		return Efallthrough;
	if (strcmp(type, "complete") == 0) {

		p = home_call_ret(pane, ci->home, "doc:attach-view", ci->focus,
				  ci->num, NULL, "invisible");
		if (p)
			p = call_ret(pane, "attach-view", p);
		if (p)
			p = call_ret(pane, "attach-render-format", p);
		if (p) {
			attr_set_str(&p->attrs, "line-format", "%+name%suffix");
			attr_set_str(&p->attrs, "heading", "");
			attr_set_str(&p->attrs, "done-key", "Replace");
			p = call_ret(pane, "attach-render-complete", p);
		}
		if (p)
			return comm_call(ci->comm2, "callback:doc", p);
		return Efail;
	}
	/* any other type gets the default handling for the rendering */
	p = dr->rendering;
	if (!p || p->damaged & DAMAGED_CLOSED)
		p = ci->home;
	return home_call(p, ci->key, ci->focus,
			 ci->num,
			 NULL, ci->str,
			 p == ci->home ? (int)(unsigned long)dir_attach_func : 0,
			 NULL, NULL,
			 0, 0, ci->comm2);
}

DEF_CMD(dir_notify_close)
{
	struct doc *d = ci->home->data;
	struct directory *dr = container_of(d, struct directory, doc);

	if (ci->focus == dr->rendering) {
		dr->rendering = NULL;
		/* one out - all out ! - but it isn't safe
		 * to close yet, because that pane might have
		 * active views FIXME.
		 */
		//pane_close(ci->home);
	}
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &dir_new, 0, NULL, "attach-doc-dir");
	call_comm("global-set-command", ed, &dir_new2, 0, NULL, "open-doc-dir");

	doc_map = key_alloc();
	key_add_chain(doc_map, doc_default_cmd);

	key_add(doc_map, "doc:load-file", &dir_load_file);
	key_add(doc_map, "doc:same-file", &dir_same_file);
	key_add(doc_map, "doc:set-ref", &dir_set_ref);
	key_add(doc_map, "doc:get-attr", &dir_doc_get_attr);
	key_add(doc_map, "doc:step", &dir_step);
	key_add(doc_map, "doc:replace", &dir_cmd);
	key_add(doc_map, "doc:attach-view", &dir_attach);
	key_add(doc_map, "doc:notify:doc:revisit", &dir_revisited);

	key_add(doc_map, "get-attr", &dir_get_attr);
	key_add(doc_map, "Notify:Close", &dir_notify_close);
	key_add(doc_map, "Close", &dir_destroy);
	key_add(doc_map, "Free", &edlib_do_free);
}
