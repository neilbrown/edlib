/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
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
	unsigned int	ignore;
};
#define DOC_SHARESREF
#define DOC_DATA_TYPE struct directory
#define DOC_NEXT(d,m,r,b) dir_next(d,r,b)
#define DOC_PREV(d,m,r,b) dir_prev(d,r,b)
#include "core.h"

struct dir_ent {
	char			*name safe;
	wchar_t			ch;
	struct list_head	lst;
	struct attrset		*attrs;
	struct stat		st;
	char			nbuf[30];
};

struct directory {
	struct doc		doc;
	struct list_head	ents;

	struct stat		stat;
	char			*fname;
};
#include "core-pane.h"

static void get_stat(struct directory *dr safe, struct dir_ent *de safe);

static char *key(struct list_head *le, const void *data)
{
	if (le == NULL)
		return NULL;
	return list_entry(le, struct dir_ent, lst)->name;
}

static bool add_ent(struct list_head *lst safe, struct dirent *de safe)
{
	struct dir_ent *dre;

	if (de->d_ino == 0)
		return False;

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
	list_add(&dre->lst, lst);
	return True;
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
	sort_list(lst, key, NULL);
	closedir(dir);
}

static struct map *dir_map;
DEF_LOOKUP_CMD(dir_handle, dir_map);

DEF_CMD(dir_new)
{
	struct directory *dr;
	struct pane *p;

	p = doc_register(ci->home, &dir_handle.c);
	if (!p)
		return Efail;
	dr = p->doc_data;
	INIT_LIST_HEAD(&dr->ents);
	dr->fname = NULL;

	return comm_call(ci->comm2, "callback:doc", p);
}

DEF_CMD(dir_new2)
{
	if (ci->num2 != S_IFDIR)
		return Efallthrough;
	return dir_new_func(ci);
}

DEF_CMD(dir_load_file)
{
	int fd = ci->num2;
	const char *name = ci->str;
	struct directory *dr = ci->home->doc_data;
	struct list_head new;
	struct dir_ent *de1, *de2;
	struct mark *prev, *m;
	int doclose = 0;

	prev = NULL;
	m = mark_new(ci->home);
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
			     m2 = mark_next(m2))
				m2->ref.d = de1;
			attr_free(&de->attrs);
			free(de->name);
			list_del(&de->lst);
			free(de);
			if (!prev) {
				prev = mark_dup(m);
				doc_prev(ci->home, prev);
			}
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
				doc_prev(ci->home, prev);
			}
		} else if (de1 && de2) {
			/* de1 and de2 are the same.  Just step over de1 and
			 * delete de2
			 */
			bool changed = False;
			if (de1->st.st_mode) {
				/* Need to check if stat info has changed */
				get_stat(dr, de1);
				if (de1->st.st_mode != de2->st.st_mode ||
				    de1->st.st_size != de2->st.st_size ||
				    de1->st.st_mtime != de2->st.st_mtime ||
				    de1->st.st_ctime != de2->st.st_ctime) {
					changed = True;
					de1->st = de2->st;
				}
			}
			if (changed) {
				if (!prev)
					prev = mark_dup(m);
			} else if (prev) {
				pane_notify("doc:replaced", ci->home,
					    0, prev, NULL,
					    0, m);
				mark_free(prev);
				prev = NULL;
			}
			doc_next(ci->home, m);
			mark_step_sharesref(m,0);

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
	struct directory *dr = ci->home->doc_data;
	struct stat st;

	if (ci->num <= 0)
		/* Being buried, not visited */
		return Efallthrough;

	if (stat(dr->fname, &st) == 0 &&
	    (st.st_ino != dr->stat.st_ino ||
	     st.st_dev != dr->stat.st_dev ||
	     st.st_mtime != dr->stat.st_mtime ||
	     st.st_mtim.tv_nsec != dr->stat.st_mtim.tv_nsec)) {
		char *msg = NULL;
		call("doc:load-file", ci->home, 2, NULL, NULL, -1);
		asprintf(&msg, "Directory %s reloaded", dr->fname);
		call("Message", ci->focus, 0, NULL, msg);
		free(msg);
	}
	return Efallthrough;
}

DEF_CMD(dir_same_file)
{
	int fd = ci->num2;
	struct stat stb;
	struct directory *dr = ci->home->doc_data;

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

static inline wint_t dir_next(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct directory *dr = p->doc_data;
	struct dir_ent *d = r->d;

	if (d == NULL)
		return WEOF;
	else {
		if (d == list_last_entry(&dr->ents,
					 struct dir_ent, lst))
			d = NULL;
		else
			d = list_next_entry(d, lst);
	}
	r->d = d;
	return '\n';
}

static inline wint_t dir_prev(struct pane *p safe, struct doc_ref *r safe, bool bytes)
{
	struct directory *dr = p->doc_data;
	struct dir_ent *d = r->d;

	if (d == list_first_entry(&dr->ents, struct dir_ent, lst))
		d = NULL;
	else if (d == NULL)
		d = list_last_entry(&dr->ents, struct dir_ent, lst);
	else
		d = list_prev_entry(d, lst);
	if (!d)
		return WEOF;

	r->d = d;
	return '\n';
}

DEF_CMD(dir_char)
{
	return do_char_byte(ci);
}

DEF_CMD(dir_set_ref)
{
	struct directory *dr = ci->home->doc_data;
	struct mark *m = ci->mark;

	if (!m)
		return Enoarg;

	mark_to_end(ci->home, m, ci->num != 1);
	if (list_empty(&dr->ents) || ci->num != 1)
		m->ref.d = NULL;
	else
		m->ref.d = list_first_entry(&dr->ents, struct dir_ent, lst);
	m->ref.ignore = 0;
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

static char *fmt_date(struct dir_ent *de safe, time_t t, struct pane *p safe)
{
	struct tm tm;
	time_t now = time(NULL);

	if (edlib_testing(p)) {
		t = 1581382278;
		now = t;
	}
	localtime_r(&t, &tm);
	if (t > now || t < now - 10*30*24*3600)
		strftime(de->nbuf, sizeof(de->nbuf),
			 "%b %d  %Y", &tm);
	else
		strftime(de->nbuf, sizeof(de->nbuf),
			 "%b %d %H:%M", &tm);
	return de->nbuf;
}

static char *fmt_size(struct dir_ent *de safe, loff_t size)
{
	if (size < 1024)
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ld", size);
	else if (size < 1024*10)
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ld.%02ldK", size/1024, (size%1023)*100 / 1024);
	else if (size < 1024*1024)
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ldK", size/1024);
	else if (size < 1024L*1024*10)
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ld.%02ldM", size/1024/1024, ((size/1024)%1023)*100 / 1024);
	else if (size < 1024L*1024*1024)
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ldM", size/1024/1024);
	else if (size < 1024L*1024*1024*10)
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ld.%02ldG", size/1024/1024/1024, ((size/1024/1024)%1023)*100 / 1024);
	else
		snprintf(de->nbuf, sizeof(de->nbuf),
			"%ldG", size/1024/1024/1024);
	return de->nbuf;
}

static char *pwname(int uid)
{
	static int last_uid = -1;
	static char *last_name = NULL;
	struct passwd *pw;

	if (uid != last_uid) {
		free(last_name);
		pw = getpwuid(uid);
		if (pw && pw->pw_name)
			last_name = strdup(pw->pw_name);
		else
			last_name = NULL;
		last_uid = uid;
	}
	return last_name;
}

static char *grname(int gid)
{
	static int last_gid = -1;
	static char *last_name = NULL;
	struct group *gr;

	if (gid != last_gid) {
		free(last_name);
		gr = getgrgid(gid);
		if (gr && gr->gr_name)
			last_name = strdup(gr->gr_name);
		else
			last_name = NULL;
		last_gid = gid;
	}
	return last_name;
}

static const char *_dir_get_attr(struct pane *home safe, struct mark *m safe,
				  const char *attr safe)

{
	struct directory *dr = home->doc_data;
	struct dir_ent *de;

	de = m->ref.d;
	if (!de)
		return NULL;
	if (strcmp(attr, "name") == 0) {
		return de->name;
	} else if (strcmp(attr, "type") == 0) {
		de->nbuf[0] = de->ch;
		de->nbuf[1] = 0;
		return de->nbuf;
	} else if (strcmp(attr, "size") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_size);
	} else if (strcmp(attr, "hsize") == 0) {
		get_stat(dr, de);
		if (strchr(".:d", de->ch) &&
		    edlib_testing(home))
			/* Size might not be reliable for testing */
			return "DIR";
		return fmt_size(de, de->st.st_size);
	} else if (strcmp(attr, "mtime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_mtime);
	} else if (strcmp(attr, "mdate") == 0) {
		get_stat(dr, de);
		return fmt_date(de, de->st.st_mtime, home);
	} else if (strcmp(attr, "atime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_atime);
	} else if (strcmp(attr, "adate") == 0) {
		get_stat(dr, de);
		return fmt_date(de, de->st.st_atime, home);
	} else if (strcmp(attr, "ctime") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_ctime);
	} else if (strcmp(attr, "cdate") == 0) {
		get_stat(dr, de);
		return fmt_date(de, de->st.st_ctime, home);
	} else if (strcmp(attr, "uid") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_uid);
	} else if (strcmp(attr, "gid") == 0) {
		get_stat(dr, de);
		return fmt_num(de, de->st.st_gid);
	} else if (strcmp(attr, "user") == 0) {
		char *n;
		get_stat(dr, de);
		if (edlib_testing(home))
			return "User";
		n = pwname(de->st.st_uid);
		if (n)
			return save_str(de, n);
		else
			return fmt_num(de, de->st.st_uid);
	} else if (strcmp(attr, "group") == 0) {
		char *n;
		get_stat(dr, de);
		if (edlib_testing(home))
			return "Group";
		n = grname(de->st.st_gid);
		if (n)
			return save_str(de, n);
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
		if (edlib_testing(home) && de->ch == ':')
			/* ".." might not be under control of the test */
			mode = 0777;
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
	} else if (strcmp(attr, "arrow") == 0) {
		if (strchr("lL", de->ch))
			return " -> ";
		else
			return "";
	} else if (strcmp(attr, "target") == 0) {
		int dfd;
		char buf[PATH_MAX];
		int len;

		if (strchr("lL", de->ch) == NULL)
			return "";
		dfd = open(dr->fname, O_RDONLY);
		if (dfd < 0)
			return "";
		len = readlinkat(dfd, de->name, buf, sizeof(buf));
		close(dfd);
		if (len <= 0 || len >= (int)sizeof(buf))
			return "";
		buf[len] = 0;
		return strsave(home, buf);
	} else
		return attr_find(de->attrs, attr);
}

DEF_CMD(dir_doc_get_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val;

	if (!m || !attr)
		return Enoarg;
	val = _dir_get_attr(ci->home, m, attr);

	if (!val)
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, m, val,
		  0, NULL, attr);
	return 1;
}

DEF_CMD(dir_doc_set_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val = ci->str2;

	if (!m || !attr)
		return Enoarg;
	if (!m->ref.d)
		return Einval;
	attr_set_str(&m->ref.d->attrs, attr, val);
	pane_notify("doc:replaced-attr", ci->home, 1, ci->mark);
	return 1;
}

DEF_CMD(dir_get_attr)
{
	struct directory *dr = ci->home->doc_data;
	const char *attr = ci->str;
	const char *val;

	if (!attr)
		return Enoarg;

	if ((val = attr_find(ci->home->attrs, attr)) != NULL)
		;
	else if (strcmp(attr, "heading") == 0)
		val = "File Name";
	else if (strcmp(attr, "render-default") == 0)
		val = "format";
	else if (strcmp(attr, "render-simple") == 0)
		val = "format";
	else if (strcmp(attr, "view-default") == 0)
		val = "dirview";
	else if (strcmp(attr, "doc-type") == 0)
		val = "dir";
	else if (strcmp(attr, "line-format") == 0)
		val = "%name";
	else if (strcmp(attr, "filename") == 0)
		val = dr->fname;
	else
		return Efallthrough;
	comm_call(ci->comm2, "callback:get_attr", ci->focus, 0, NULL, val);
	return 1;
}

DEF_CMD(dir_val_marks)
{
	struct directory *dr = ci->home->doc_data;
	struct dir_ent *de;
	int found;

	if (!ci->mark || !ci->mark2)
		return Enoarg;

	if (ci->mark->ref.d == ci->mark2->ref.d) {
		if (ci->mark->ref.ignore < ci->mark2->ref.ignore)
			return 1;
		LOG("dir_val_marks: same buf, bad offset: %u, %u",
		    ci->mark->ref.ignore, ci->mark2->ref.ignore);
		return Efalse;
	}
	if (ci->mark->ref.d == NULL) {
		LOG("dir_val_marks: mark.d is NULL");
		return Efalse;
	}
	found = 0;
	list_for_each_entry(de, &dr->ents, lst) {
		if (ci->mark->ref.d == de)
			found = 1;
		if (ci->mark2->ref.d == de) {
			if (found == 1)
				return 1;
			LOG("dir_val_marks: mark2.d found before mark1");
			return Efalse;
		}
	}
	if (ci->mark2->ref.d == NULL) {
		if (found == 1)
			return 1;
		LOG("dir_val_marks: mark2.d (NULL) found before mark1");
		return Efalse;
	}
	if (found == 0)
		LOG("dir_val_marks: Neither mark found in de list");
	if (found == 1)
		LOG("dir_val_marks: mark2 not found in de list");
	return Efalse;
}

DEF_CMD_CLOSED(dir_destroy)
{
	struct directory *dr = ci->home->doc_data;

	while (!list_empty(&dr->ents)) {
		struct dir_ent *de = list_entry(dr->ents.next,
						struct dir_ent, lst);

		attr_free(&de->attrs);
		free(de->name);
		list_del(&de->lst);
		free(de);
	}
	return Efallthrough;
}

DEF_CMD(dir_shares_ref)
{
	return 1;
}

DEF_CMD(dir_debug_mark)
{
	char *ret = NULL;
	struct mark *m = ci->mark;
	struct dir_ent *de;

	if (!m || m->owner != ci->home || !ci->comm2)
		return Enoarg;
	de = m->ref.d;
	if (!mark_valid(m))
		ret = strdup("M:FREED");
	else if (!de)
		ret = strdup("M:EOF");
	else
		asprintf(&ret, "M:%s(#%x)", de->name, m->ref.ignore);
	comm_call(ci->comm2, "cb", ci->focus, 0, NULL, ret);
	free(ret);
	return 1;
}

static struct map *dirview_map;
DEF_LOOKUP_CMD(dirview_handle, dirview_map);

static int dir_open(struct pane *focus safe,
		    struct mark *m, bool other, bool follow)
{
	/* close this pane, open the given file. */

	struct pane *par, *p;
	int fd;
	char *dirname, *basename, *type;
	char *fname = NULL;

	if (!m)
		m = call_ret(mark, "doc:point", focus);
	if (!m)
		return Enoarg;
	dirname = pane_attr_get(focus, "filename");
	basename = pane_mark_attr(focus, m, "name");
	type = pane_mark_attr(focus, m, "type");
	if (!dirname || !basename || !type)
		return Efail;

	asprintf(&fname, "%s/%s", dirname, basename);
	if (!fname)
		return Efail;

	if (follow && (type[0] == 'l' || type[0] == 'L')) {
		/* Fname is a symlink.  Read it and open
		 * that directly.  Only follow this step once.
		 */
		char path[PATH_MAX];
		int ret;

		ret = readlink(fname, path, sizeof(path));
		if (ret > 0 && ret < (int)sizeof(path)) {
			path[ret] = 0;
			if (fname[0] == '/')
				asprintf(&fname, "%s", path);
			else
				asprintf(&fname, "%s/%s", dirname, path);
			if (!fname)
				return Efail;
		}
	}
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
	if (other) {
		par = home_call_ret(pane, focus, "DocPane", p);
		if (par) {
			pane_take_focus(par);
			return 1;
		}
		par = call_ret(pane, "OtherPane", focus);
	} else
		par = call_ret(pane, "ThisPane", focus);
	if (par) {
		p = home_call_ret(pane, p, "doc:attach-view", par);
		pane_take_focus(p);
	}
	return 1;
}

static int dir_open_alt(struct pane *focus safe,
			struct mark *m, char cmd)
{
	/* close this pane, open the given file. */
	struct pane *p;
	int fd;
	char *dirname, *basename;
	char *fname = NULL;
	char buf[100];

	if (!m)
		return Enoarg;
	dirname = pane_attr_get(focus, "filename");
	basename = pane_mark_attr(focus, m, "name");
	if (!dirname || !basename)
		return Efail;

	asprintf(&fname, "%s/%s", dirname, basename);
	if (!fname)
		return Efail;
	fd = open(fname, O_RDONLY);

	if (fd >= 0) {
		struct pane *new = call_ret(pane, "doc:open", focus,
					    fd, NULL, fname);
		close(fd);
		if (!new)
			return Efail;
		snprintf(buf, sizeof(buf), "cmd-%c", cmd);
		p = call_ret(pane, "ThisPane", focus);
		if (!p)
			return Efail;

		p = home_call_ret(pane, new, "doc:attach-view", p,
				  1, NULL, buf);
	} else {
		struct pane *doc = call_ret(pane, "doc:from-text", focus,
					    0, NULL, fname,
					    0, NULL, "File not found\n");
		if (!doc)
			return Efail;
		p = call_ret(pane, "ThisPane", focus);
		if (!p)
			return Efail;
		p = home_call_ret(pane, doc, "doc:attach-view", p, 1);
	}
	free(fname);
	pane_take_focus(p);
	return 1;
}

DEF_CMD(dir_do_open)
{
	return dir_open(ci->focus, ci->mark, False, ci->num == 1);
}

DEF_CMD(dir_do_open_other)
{
	return dir_open(ci->focus, ci->mark, True, ci->num == 1);
}

DEF_CMD(dir_do_reload)
{
	return call("doc:load-file", ci->focus, 0, NULL, NULL, -1);
}

DEF_CMD(dir_do_mark_del)
{
	call("doc:set-attr", ci->focus, 0, ci->mark, "dir-cmd",
	     0, NULL, "D");
	call("doc:EOL", ci->focus, 1, ci->mark, NULL, 1);
	return 1;
}

DEF_CMD(dir_do_mark)
{
	call("doc:set-attr", ci->focus, 0, ci->mark, "dir-cmd",
	     0, NULL, "*");
	call("doc:EOL", ci->focus, 1, ci->mark, NULL, 1);
	return 1;
}

DEF_CMD(dir_un_mark)
{
	call("doc:set-attr", ci->focus, 0, ci->mark, "dir-cmd",
	     0, NULL, NULL);
	call("doc:EOL", ci->focus, 1, ci->mark, NULL, 1);
	return 1;
}

DEF_CMD(dir_un_mark_back)
{
	call("doc:EOL", ci->focus, -2, ci->mark);
	call("doc:set-attr", ci->focus, 0, ci->mark, "dir-cmd",
	     0, NULL, NULL);
	return 1;
}

DEF_CMD(dir_do_quit)
{
	return call("doc:destroy", ci->home);
}

DEF_CMD(dir_do_special)
{
	const char *c = ksuffix(ci, "doc:cmd-");

	return dir_open_alt(ci->focus, ci->mark, c[0]);
}

static void add_name(struct buf *b safe, char *name safe)
{
	if (strchr(name, '\'') == NULL) {
		buf_append(b, ' ');
		buf_append(b, '\'');
		buf_concat(b, name);
		buf_append(b, '\'');
	} else {
		buf_append(b, ' ');
		buf_append(b, '"');
		while (*name) {
			if (strchr("\"$`\\", *name))
				buf_append(b, '\\');
			buf_append_byte(b, *name);
			name += 1;
		}
		buf_append(b, '"');
	}
}

static char *collect_names(struct pane *p safe, char *type,
			   struct mark *mark)
{
	struct mark *m = mark_new(p);
	struct buf b;

	if (!m)
		return NULL;
	buf_init(&b);
	while (type && doc_following(p, m) != WEOF) {
		char *t, *name;
		t = pane_mark_attr(p, m, "dir-cmd");
		if (!t  || strcmp(t, type) != 0) {
			doc_next(p, m);
			continue;
		}
		name = pane_mark_attr(p, m, "name");
		call("doc:set-attr", p, 0, m, "dir-cmd");
		doc_next(p, m);
		if (!name)
			continue;
		add_name(&b, name);
	}
	mark_free(m);
	if (!b.len && mark) {
		char *name = pane_mark_attr(p, mark, "name");
		if (name)
			add_name(&b, name);
	}
	return buf_final(&b);
}

DEF_CMD(dir_expunge)
{
	char *names, *cmd;
	struct pane *p;

	names = collect_names(ci->focus, "D", NULL);

	if (!names || !names[0]) {
		free(names);
		call("Message:modal", ci->focus, 0, NULL,
		     "No files marked for deletion");
		return Efail;
	}
	cmd = strconcat(ci->focus, "rm -f ", names);
	p = call_ret(pane, "attach-shell-prompt", ci->focus, 0, NULL, cmd);
	if (p) {
		// put cursor after the "-f"
		call("doc:file", p, -1);
		call("doc:char", p, 5);
		pane_add_notify(ci->home, p, "Notify:Close");
	}
	free(names);
	return 1;
}

DEF_CMD(dir_chmodown)
{
	char *names, *cmd;
	struct pane *p;
	char *which;

	if (strcmp(ci->key, "K:A-o") == 0)
		which = "chown";
	else
		which = "chmod";

	names = collect_names(ci->focus, "*", ci->mark);
	if (!names || !*names) {
		free(names);
		call("Message:modal", ci->focus, 0, NULL,
		     strconcat(ci->focus, "No file for ", which));
		return Efail;
	}
	cmd = strconcat(ci->focus, which, " ", names);
	p = call_ret(pane, "attach-shell-prompt", ci->focus, 0, NULL, cmd);
	if (p) {
		call("doc:file", p, -1);
		call("doc:char", p, strlen(which) + 1);
		pane_add_notify(ci->home, p, "Notify:Close");
	}
	free(names);
	return 1;
}

DEF_CMD(dir_rename)
{
	char *names, *cmd;
	struct pane *p;
	char *dirname = pane_attr_get(ci->focus, "dirname");
	int prefix;

	if (!dirname)
		dirname = ".";

	names = collect_names(ci->focus, "*", NULL);
	if (names && *names) {
		cmd = strconcat(ci->focus, "mv --target-directory ",
				dirname, names);
		prefix = strlen(cmd) - strlen(names);
	} else {
		free(names);
		names = collect_names(ci->focus, NULL, ci->mark);
		if (!names || !*names) {
			free(names);
			call("Message:modal", ci->focus, 0, NULL,
			     "No file for rename");
			return Efail;
		}
		cmd = strconcat(ci->focus, "mv", names, " ", dirname);
		prefix = strlen(cmd);
	}
	p = call_ret(pane, "attach-shell-prompt", ci->focus, 0, NULL, cmd);
	if (p) {
		call("doc:file", p, -1);
		call("doc:char", p, prefix);
		pane_add_notify(ci->home, p, "Notify:Close");
	}
	free(names);
	return 1;
}

DEF_CMD(dirview_attach)
{
	struct pane *p;

	p = call_ret(pane, "attach-viewer", ci->focus);
	if (!p)
		p = ci->focus;
	p = pane_register(p, 0, &dirview_handle.c);
	if (!p)
		return Efail;
	attr_set_str(&p->attrs, "line-format",
		     "<fg:green-40>%flag</> <fg:red>%perms</> %mdate:13 %user:10 %group:10%hsize:-6  <fg:blue>%name%suffix</>%arrow<fg:green-30>%target</>");
	attr_set_str(&p->attrs, "heading",
		     "<bold,fg:blue,underline>  Perms      Mtime         Owner      Group      Size   File Name</>");

	comm_call(ci->comm2, "cb", p);
	return 1;
}

DEF_CMD(dirview_clone)
{
	struct pane *p;

	p = pane_register(ci->focus, 0, &dirview_handle.c);
	if (!p)
		return Efail;

	pane_clone_children(ci->home, p);
	return 1;
}

DEF_CMD(dirview_doc_get_attr)
{
	struct mark *m = ci->mark;
	const char *attr = ci->str;
	const char *val;

	if (!m || !attr)
		return Enoarg;
	if (strcmp(attr, "flag") != 0)
		return Efallthrough;
	val = pane_mark_attr(ci->home->parent, m, "dir-cmd");
	if (!val)
		val = " ";
	comm_call(ci->comm2, "cb", ci->focus, 0, m, val, 0, NULL, attr);
	return 1;
}

DEF_CMD(dirview_close_notify)
{
	/* shell window closed, maybe something was changed - check */
	if (ci->key[0] == 'c') {
		/* the callback */
		call("doc:notify:doc:revisit", ci->focus, 1);
		return 1;
	}
	call_comm("event:timer", ci->home, &dirview_close_notify, 500);
	return 1;
}

void edlib_init(struct pane *ed safe)
{
	call_comm("global-set-command", ed, &dir_new, 0, NULL, "attach-doc-dir");
	call_comm("global-set-command", ed, &dir_new2, 0, NULL, "open-doc-dir");

	dir_map = key_alloc();
	key_add_chain(dir_map, doc_default_cmd);

	key_add(dir_map, "doc:load-file", &dir_load_file);
	key_add(dir_map, "doc:same-file", &dir_same_file);
	key_add(dir_map, "doc:set-ref", &dir_set_ref);
	key_add(dir_map, "doc:get-attr", &dir_doc_get_attr);
	key_add(dir_map, "doc:set-attr", &dir_doc_set_attr);
	key_add(dir_map, "doc:char", &dir_char);
	key_add(dir_map, "doc:notify:doc:revisit", &dir_revisited);
	key_add(dir_map, "doc:debug:mark", &dir_debug_mark);

	key_add(dir_map, "doc:shares-ref", &dir_shares_ref);

	key_add(dir_map, "get-attr", &dir_get_attr);
	key_add(dir_map, "Close", &dir_destroy);
	if(0)key_add(dir_map, "debug:validate-marks", &dir_val_marks);

	call_comm("global-set-command", ed, &dirview_attach, 0, NULL,
		  "attach-dirview");

	dirview_map = key_alloc();
	key_add(dirview_map, "doc:cmd-f", &dir_do_open);
	key_add(dirview_map, "doc:cmd-o", &dir_do_open_other);
	key_add(dirview_map, "doc:cmd-\n", &dir_do_open);
	key_add(dirview_map, "doc:cmd:Enter", &dir_do_open);
	key_add(dirview_map, "doc:cmd-g", &dir_do_reload);
	key_add(dirview_map, "doc:cmd-q", &dir_do_quit);
	key_add(dirview_map, "doc:cmd-d", &dir_do_mark_del);
	key_add(dirview_map, "doc:cmd-m", &dir_do_mark);
	key_add(dirview_map, "doc:cmd-u", &dir_un_mark);
	key_add(dirview_map, "doc:cmd-x", &dir_expunge);
	key_add(dirview_map, "doc:cmd-r", &dir_rename);
	key_add(dirview_map, "K:A-o", &dir_chmodown);
	key_add(dirview_map, "K:A-m", &dir_chmodown);
	key_add(dirview_map, "K:Del", &dir_un_mark_back);
	key_add_range(dirview_map, "doc:cmd-A", "doc:cmd-Z", &dir_do_special);
	key_add(dirview_map, "doc:get-attr", &dirview_doc_get_attr);
	key_add(dirview_map, "Clone", &dirview_clone);
	key_add(dirview_map, "Notify:Close", &dirview_close_notify);
}
