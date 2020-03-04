/*
 * Copyright Neil Brown ©2015-2019 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Python3 bindings for edlib.
 * And edlib command "python-load" will read and execute a python
 * script.
 * It can use "edlib.editor" to get the editor instance, and can
 * use "edlib.call()" to issue edlib commands.
 *
 * Types available are:
 *  edlib.pane  - a generic pane.  These form a tree of which edlib.editor
 *                is the root.
 *                get/set operations for x,y,z,w,h,cx,cy as numbers
 *                     changes x,y,w,h call pane_resize()
 *                     z cannot be changed (only pane_register() does that )
 *                     cx,cy can be changed freely.
 *                  'parent' and 'focus' can be read but not written
 *                method for 'children' provides first child as an iterator.
 *                method abs() converts relative co-ords to absolute.
 *                rel() converts absolute co-ords to relative.
 *  edlib.mark  - these reference locations in a document.  The document is
 *                not directly accessible, it can only be accessed through
 *                a pane (which may translate events and results).
 *                get/set for viewnum (cannot be set)
 *                iterator for both 'all' and 'view' lists.
 *                no methods yet.
 *
 *  edlib.comm  - a command which can be used to invoke code in other
 *                module editors.  These look like any other python
 *                callable.  They cannot be explicitly created, but can
 *                be received from and passed to other commands.
 *
 */

#ifdef __CHECKER__
/* Need to define stuff that pyconfig needs */
#define __linux__
#define __x86_64__
#define __LP64__
#endif
#include <Python.h>
#include <structmember.h>
#define MARK_DATA_PTR PyObject
#define PRIVATE_DOC_REF

struct doc_ref {
	PyObject *c;
	int o;
};
#include "core.h"
#include "misc.h"

#define SAFE_CI {.key=safe_cast NULL,\
			.home=safe_cast NULL,\
			.focus=safe_cast NULL,\
			.comm=safe_cast NULL,\
			}

static PyObject *Edlib_CommandFailed;
static PyObject *EdlibModule;

static char *module_dir;

/* When a python callable is passed to edlib_call() we combine it
 * with this "python_call" for edlib to call back that callable.
 */
struct python_command {
	struct command	c;
	PyObject	*callable;
};
DEF_CMD(python_call);
DEF_CMD(python_pane_call);
DEF_CMD(python_doc_call);
static void python_free_command(struct command *c safe);

typedef struct {
	PyObject_HEAD
	struct pane	*pane;
	struct python_command handle;
	struct map	*map;
	int		map_init;
	PyObject	*refer;
} Pane;
static PyTypeObject PaneType;

typedef struct {
	PyObject_HEAD
	struct pane	*pane;
} PaneIter;
static PyTypeObject PaneIterType;

typedef struct {
	PyObject_HEAD
	struct pane	*pane;
	struct python_command handle;
	struct map	*map;
	int		map_init;
	PyObject	*refer;
	struct doc	doc;
} Doc;
static PyTypeObject DocType;

typedef struct {
	PyObject_HEAD
	struct mark	*mark;
} Mark;
static PyTypeObject MarkType;
static void mark_refcnt(struct mark *m safe, int inc);

typedef struct {
	PyObject_HEAD
	struct command	*comm;
} Comm;
static PyTypeObject CommType;

static inline int pane_valid(Pane *p safe)
{
	if (p->pane && p->pane->handle)
		return 1;
	PyErr_SetString(PyExc_TypeError, "Pane has been freed");
	return 0;
}

static inline int doc_valid(Doc *p safe)
{
	if (p->pane && p->pane->handle)
		return 1;
	PyErr_SetString(PyExc_TypeError, "Doc pane has been freed");
	return 0;
}

static int get_cmd_info(struct cmd_info *ci safe, PyObject *args safe, PyObject *kwds,
			PyObject **s1 safe, PyObject **s2 safe);

static int in_pane_frompane = 0;
static inline PyObject *safe Pane_Frompane(struct pane *p)
{
	Pane *pane;
	if (p && p->handle && p->handle->func == python_pane_call.func) {
		pane = p->data;
		Py_INCREF(pane);
	} else if (p && p->handle && p->handle->func == python_doc_call.func) {
		struct doc *doc = p->data;
		Doc *pdoc = container_of(doc, Doc, doc);
		pane = (Pane*)pdoc;
		Py_INCREF(pane);
	} else {
		in_pane_frompane = 1;
		pane = (Pane * safe)PyObject_CallObject((PyObject*)&PaneType, NULL);
		in_pane_frompane = 0;
		pane->pane = p;
	}
	return (PyObject*)pane;
}

static inline PyObject *safe Mark_Frommark(struct mark *m safe)
{
	Mark *mark;

	if (m->mtype == &MarkType && m->mdata) {
		Py_INCREF(m->mdata);
		return m->mdata;
	}
	mark = (Mark * safe)PyObject_CallObject((PyObject*)&MarkType, NULL);
	mark->mark = m;
	return (PyObject*)mark;
}

static inline PyObject *safe Comm_Fromcomm(struct command *c safe)
{
	if (c->func == python_call_func && 0) {
		struct python_command *pc = container_of(c, struct python_command, c);
		if (!pc->callable)
			return NULL;
		Py_INCREF(pc->callable);
		return pc->callable;
	} else {
		Comm *comm = (Comm*safe)PyObject_CallObject((PyObject*)&CommType, NULL);
		comm->comm = command_get(c);
		return (PyObject*)comm;
	}
}

DEF_CMD(python_load)
{
	const char *fname = ci->str;
	FILE *fp;
	PyObject *globals, *main_mod;
	PyObject *Ed;

	if (!fname)
		return Enoarg;
	fp = fopen(fname, "r");
	if (!fp)
		return Efail;

	main_mod = PyImport_AddModule("__main__");
	if (main_mod == NULL)
		return Einval;
	globals = PyModule_GetDict(main_mod);

	Ed = Pane_Frompane(ci->home);
	PyDict_SetItemString(globals, "editor", Ed);
	PyDict_SetItemString(globals, "edlib", EdlibModule);
	PyRun_FileExFlags(fp, fname, Py_file_input, globals, globals, 0, NULL);
	PyErr_Print();
	Py_DECREF(Ed);
	fclose(fp);
	return 1;
}

DEF_CMD(python_load_module)
{
	const char *name = ci->str;
	FILE *fp;
	PyObject *globals, *main_mod;
	PyObject *Ed;
	char buf [PATH_MAX];

	if (!name)
		return Enoarg;
	snprintf(buf, sizeof(buf), "%s/python/%s.py", module_dir, name);
	fp = fopen(buf, "r");
	if (!fp)
		return Efail;

	main_mod = PyImport_AddModule("__main__");
	if (main_mod == NULL)
		return Einval;
	globals = PyModule_GetDict(main_mod);

	Ed = Pane_Frompane(ci->home);
	PyDict_SetItemString(globals, "editor", Ed);
	PyDict_SetItemString(globals, "pane", Pane_Frompane(ci->focus));
	PyDict_SetItemString(globals, "edlib", EdlibModule);
	PyRun_FileExFlags(fp, buf, Py_file_input, globals, globals, 0, NULL);
	PyErr_Print();
	Py_DECREF(Ed);
	fclose(fp);
	return 1;
}

static PyObject *safe python_string(const char *s safe)
{
	const char *c = s;
	while (*c && !(*c & 0x80))
		c++;
	if (*c)
		/* must be Unicode */
		return safe_cast PyUnicode_DecodeUTF8(s, strlen(s), NULL);
	else
		return safe_cast Py_BuildValue("s", s);
}

static char *python_as_string(PyObject *s, PyObject **tofree safe)
{
	if (s && PyUnicode_Check(s)) {
		s = PyUnicode_AsUTF8String(s);
		*tofree = s;
	}
	if (s && PyBytes_Check(s)) {
		char *ret = PyBytes_AsString(s);
		unsigned char *r = (unsigned char*)ret;
		if (r && r[0] == 0xef && r[1] == 0xbb && r[2] == 0xbf)
			/* UTF-8 Byte Order Mark */
			return ret+3;
		else
			return ret;
	}
	return NULL;
}

static int dict_add(PyObject *kwds, char *name, PyObject *val)
{
	if (!val)
		return 0;
	PyDict_SetItemString(kwds, name, val);
	Py_DECREF(val);
	return 1;
}

REDEF_CMD(python_call)
{
	struct python_command *pc = container_of(ci->comm, struct python_command, c);
	PyObject *ret = NULL, *args, *kwds;
	int rv = 1;

	args = safe_cast Py_BuildValue("(s)", ci->key);
	kwds = PyDict_New();
	rv = rv && dict_add(kwds, "home", Pane_Frompane(ci->home));
	rv = rv && dict_add(kwds, "focus",
			    Pane_Frompane(ci->focus));
	rv = rv && dict_add(kwds, "mark",
			    ci->mark ? Mark_Frommark(ci->mark):
			    (Py_INCREF(Py_None), Py_None));
	rv = rv && dict_add(kwds, "mark2",
			    ci->mark2 ? Mark_Frommark(ci->mark2):
			    (Py_INCREF(Py_None), Py_None));
	rv = rv && dict_add(kwds, "str",
			    ci->str ? python_string(ci->str):
			    (Py_INCREF(Py_None), safe_cast Py_None));
	rv = rv && dict_add(kwds, "str2",
			    ci->str2 ? python_string(ci->str2):
			    (Py_INCREF(Py_None), safe_cast Py_None));
	rv = rv && dict_add(kwds, "comm", Comm_Fromcomm(ci->comm));
	rv = rv && dict_add(kwds, "comm2",
			    ci->comm2 ? Comm_Fromcomm(ci->comm2):
			    (Py_INCREF(Py_None), Py_None));
	rv = rv && dict_add(kwds, "num",
			    Py_BuildValue("i", ci->num));
	rv = rv && dict_add(kwds, "rpt_num",
			    Py_BuildValue("i", RPT_NUM(ci)));
	rv = rv && dict_add(kwds, "num2",
			    Py_BuildValue("i", ci->num2));
	rv = rv && dict_add(kwds, "xy",
			    Py_BuildValue("ii", ci->x, ci->y));

	if (rv && pc->callable)
		ret = PyObject_Call(pc->callable, args, kwds);

	Py_DECREF(args);
	Py_DECREF(kwds);
	if (!ret) {
		PyErr_Print();
		/* FIXME cancel error?? */
		return Efail;
	}
	if (ret == Py_None)
		rv = 0;
	else if (PyLong_Check(ret))
		rv = PyLong_AsLong(ret);
	else if (PyBool_Check(ret))
		rv = (ret == Py_True);
	else if (PyUnicode_Check(ret) && PyUnicode_GET_SIZE(ret) >= 1)
		rv = CHAR_RET(PyUnicode_READ_CHAR(ret, 0));
	else
		rv = 1;
	Py_DECREF(ret);
	return rv;
}

REDEF_CMD(python_doc_call)
{
	int rv = python_pane_call_func(ci);
	if (rv == Efallthrough)
		rv = key_lookup(doc_default_cmd, ci);
	return rv;
}

static void do_map_init(Pane *self safe)
{
	int i;
	PyObject *refer = self->refer ?: (PyObject*)self;
	PyObject *l = PyObject_Dir(refer);
	int n;

	if (!self->map || !self->pane || !l)
		return;
	n = PyList_Size(l);
	for (i = 0; i < n ; i++) {
		PyObject *e = PyList_GetItem(l, i);
		PyObject *m = PyObject_GetAttr(refer, e);

		if (m && PyMethod_Check(m)) {
			PyObject *doc = PyObject_GetAttrString(m, "__doc__");
			if (doc && doc != Py_None) {
				PyObject *tofree = NULL;
				char *docs = python_as_string(doc, &tofree);
				if (docs &&
				    strncmp(docs, "handle:", 7) == 0) {
					struct python_command *comm =
						malloc(sizeof(*comm));
					comm->c = python_call;
					comm->c.free = python_free_command;
					command_get(&comm->c);
					Py_INCREF(m);
					comm->callable = m;
					key_add(self->map, docs+7, &comm->c);
					command_put(&comm->c);
				}
				if (docs &&
				    strncmp(docs, "handle-range", 12) == 0 &&
				    docs[12]) {
					char sep = docs[12];
					char *s1 = strchr(docs+13, sep);
					char *s2 = s1 ? strchr(s1+1, sep) : NULL;
					if (s2) {
						char *a = strndup(docs+13, s1-(docs+13));
						char *b = strndup(s1+1, s2-(s1+1));

						struct python_command *comm =
							malloc(sizeof(*comm));
						comm->c = python_call;
						comm->c.free = python_free_command;
						command_get(&comm->c);
						Py_INCREF(m);
						comm->callable = m;
						key_add_range(self->map, a, b,
							      &comm->c);
						free(a); free(b);
						command_put(&comm->c);
					}
				}
				if (docs &&
				    strncmp(docs, "handle-prefix:", 14) == 0) {
					char *a = strconcat(self->pane, docs+14);
					char *b = strconcat(self->pane,
							    a, "\xFF\xFF\xFF\xFF");
					struct python_command *comm =
						malloc(sizeof(*comm));
					comm->c = python_call;
					comm->c.free = python_free_command;
					command_get(&comm->c);
					Py_INCREF(m);
					comm->callable = m;
					key_add_range(self->map, a, b,
							      &comm->c);
					command_put(&comm->c);
				}
				if (docs &&
				    strncmp(docs, "handle-list", 11) == 0 &&
				    docs[11]) {
					char sep = docs[11];
					char *s1 = docs + 12;
					while (s1 && *s1 && *s1 != sep) {
						struct python_command *comm =
							malloc(sizeof(*comm));
						char *a;
						char *s2 = strchr(s1, sep);
						if (s2) {
							a = strndup(s1, s2-s1);
							s1 = s2+1;
						} else {
							a = strdup(s1);
							s1 = NULL;
						}
						comm->c = python_call;
						comm->c.free = python_free_command;
						command_get(&comm->c);
						Py_INCREF(m);
						comm->callable = m;
						key_add(self->map, a, &comm->c);
						free(a);
						command_put(&comm->c);
					}
				}
				Py_XDECREF(tofree);
			}
			Py_XDECREF(doc);
		}
		Py_XDECREF(m);
	}
	Py_XDECREF(l);
	self->map_init = 1;
}

REDEF_CMD(python_pane_call)
{
	Pane *home = container_of(ci->comm, Pane, handle.c);

	if (!home || !home->map)
		return Efallthrough;

	if (!home->map_init)
		do_map_init(home);
	return key_lookup(home->map, ci);
}

static Pane *pane_new(PyTypeObject *type safe, PyObject *args, PyObject *kwds)
{
	Pane *self;

	self = (Pane *)type->tp_alloc(type, 0);
	if (self) {
		self->pane = NULL;
	}
	return self;
}

static Doc *Doc_new(PyTypeObject *type safe, PyObject *args, PyObject *kwds)
{
	Doc *self;

	self = (Doc *)type->tp_alloc(type, 0);
	if (self) {
		self->pane = NULL;
	}
	return self;
}

static void python_pane_free(struct command *c safe)
{
	Pane *p = container_of(c, Pane, handle.c);
	/* pane has been closed */
	p->pane = NULL;
	Py_XDECREF(p->handle.callable);
	p->handle.callable = NULL;
	Py_XDECREF(p->refer);
	p->refer = NULL;
	if (p->map)
		key_free(p->map);
	p->map = NULL;
	if (PyObject_TypeCheck(p, &DocType)) {
		Doc *d = (Doc*)p;
		doc_free(&d->doc);
	}
	Py_DECREF(p);
}

static int __Pane_init(Pane *self safe, PyObject *args, PyObject *kwds,
		       Pane **parentp safe,
		       int *zp safe)
{
	Pane *parent = NULL;
	PyObject *py_handler = NULL;
	int ret;
	static char *keywords[] = {"parent", "handler", "z", NULL};

	if (self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane already initialised");
		return -1;
	}
	/* Pane(parent, handler, data, z=0 */
	if (in_pane_frompane)
		/* An internal Pane_Frompane call - it will set .pane,
		 * and we don't want a .handler.
		 */
		return 0;

	ret = PyArg_ParseTupleAndKeywords(args, kwds, "|OOi", keywords,
					  &parent, &py_handler,
					  zp);
	if (ret <= 0)
		return -1;

	if ((PyObject*)parent == Py_None)
		parent = NULL;

	if (parent && !PyObject_TypeCheck(parent, &PaneType)) {
		PyErr_SetString(PyExc_TypeError, "First arg must be edlib.Pane or None");
		return -1;
	}

	if (py_handler)
		Py_INCREF(py_handler);
	self->refer = py_handler;

	*parentp = parent;

	self->map = key_alloc();
	self->handle.c = python_pane_call;
	self->handle.c.free = python_pane_free;
	self->handle.callable = NULL;

	return 1;
}

static int Pane_init(Pane *self safe, PyObject *args, PyObject *kwds)
{
	Pane *parent = NULL;
	int z = 0;
	int ret = __Pane_init(self, args, kwds, &parent, &z);

	if (ret <= 0)
		return ret;

	/* The pane holds a reference to the Pane through the ->handle
	 * function
	 */
	Py_INCREF(self);
	self->pane = pane_register(parent ? parent->pane : NULL,
				   z, &self->handle.c, self);
	return 0;
}

static int Doc_init(Doc *self, PyObject *args, PyObject *kwds)
{
	Pane *parent = NULL;
	int z = 0;
	int ret = __Pane_init((Pane*safe)self, args, kwds, &parent, &z);

	if (ret <= 0 || !self)
		return ret;

	self->handle.c.func = python_doc_call_func;
	self->pane = doc_register(parent ? parent->pane : NULL,
				  z, &self->handle.c, self);
	self->doc.refcnt = mark_refcnt;
	return 0;
}

static inline void do_free(PyObject *ob safe)
{
	if (ob->ob_type && ob->ob_type->tp_free)
		ob->ob_type->tp_free(ob);
}

static void pane_dealloc(Pane *self safe)
{
	do_free((PyObject*safe)self);
}

static PyObject *pane_children(Pane *self safe, PyObject *args)
{
	PaneIter *ret;

	if (!pane_valid(self))
		return NULL;
	ret = (PaneIter*)PyObject_CallObject((PyObject*)&PaneIterType, NULL);
	if (ret) {
		if (list_empty(&self->pane->children))
			ret->pane = NULL;
		else
			ret->pane = list_first_entry(&self->pane->children,
						     struct pane, siblings);
	}
	return (PyObject*)ret;
}

static Pane *pane_iter_new(PyTypeObject *type safe, PyObject *args, PyObject *kwds)
{
	Pane *self;

	self = (Pane *)type->tp_alloc(type, 0);
	if (self)
		self->pane = NULL;
	return self;
}

static void paneiter_dealloc(PaneIter *self safe)
{
	do_free((PyObject*safe)self);
}

static PyObject *Pane_clone_children(Pane *self safe, PyObject *args)
{
	Pane *other = NULL;
	int ret;

	if (!pane_valid(self))
		return NULL;

	ret = PyArg_ParseTuple(args, "O!", &PaneType, &other);
	if (ret <= 0 || !other)
		return NULL;
	if (other->pane)
		pane_clone_children(self->pane, other->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_focus(Pane *self safe, PyObject *args)
{
	if (!pane_valid(self))
		return NULL;

	pane_focus(self->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_refresh(Pane *self safe, PyObject *args)
{
	if (!pane_valid(self))
		return NULL;

	pane_refresh(self->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PaneIter *pane_this_iter(PaneIter *self safe)
{
	Py_INCREF(self);
	return self;
}

static PyObject *pane_iter_next(PaneIter *self safe)
{
	PyObject *ret;
	if (!self->pane)
		/* Reached the end */
		return NULL;
	ret = Pane_Frompane(self->pane);
	if (self->pane->siblings.next == &self->pane->parent->children)
		/* Reached the end of the list */
		self->pane = NULL;
	else
		self->pane = list_next_entry(self->pane, siblings);
	return ret;
}

struct pyret {
	struct command comm;
	PyObject *ret;
};

DEF_CMD(take_focus)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);
	struct pane *p = ci->focus;

	if (!p)
		return Enoarg;
	if (pr->ret)
		return Efallthrough;
	pr->ret = Pane_Frompane(ci->focus);
	return 1;
}

DEF_CMD(take_mark)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!ci->mark)
		return Efallthrough;
	pr->ret = Mark_Frommark(ci->mark);
	return 1;
}

DEF_CMD(take_mark2)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!ci->mark2)
		return Efallthrough;
	pr->ret = Mark_Frommark(ci->mark2);
	return 1;
}

DEF_CMD(take_str)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!ci->str)
		return Efallthrough;
	pr->ret = python_string(ci->str);
	return 1;
}

static struct command *map_ret(char *ret safe)
{
	if (strcmp(ret, "focus") == 0)
		return &take_focus;
	if (strcmp(ret, "mark") == 0)
		return &take_mark;
	if (strcmp(ret, "mark2") == 0)
		return &take_mark2;
	if (strcmp(ret, "str") == 0)
		return &take_str;
	return NULL;
}

static PyObject *Pane_call(Pane *self safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2, *s3 = NULL;
	PyObject *ret;
	struct pyret pr;
	int return_char = 0;

	if (!pane_valid(self))
		return NULL;

	ci.home = self->pane;

	rv = get_cmd_info(&ci, args, kwds, &s1, &s2);

	if (rv <= 0) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}
	ret = kwds ? PyDict_GetItemString(kwds, "ret") : NULL ;
	if (ret) {
		char *rets;
		struct command *c;

		if (!PyUnicode_Check(ret) ||
		    (rets = python_as_string(ret, &s3)) == NULL) {
			PyErr_SetString(PyExc_TypeError, "ret= must be given a string");
			Py_XDECREF(s1); Py_XDECREF(s2);
			return NULL;
		}
		if (strcmp(rets, "char") == 0) {
			return_char = 1;
			ret = NULL;
		} else {
			if (ci.comm2) {
				PyErr_SetString(PyExc_TypeError, "ret= not permitted with comm2");
				Py_XDECREF(s1); Py_XDECREF(s2); Py_XDECREF(s3);
				command_put(ci.comm2);
				return NULL;
			}
			pr.ret = NULL;
			c = map_ret(rets);
			if (!c) {
				PyErr_SetString(PyExc_TypeError, "ret= type not valid");
				Py_XDECREF(s1); Py_XDECREF(s2); Py_XDECREF(s3);
				return NULL;
			}
			pr.comm = *c;
			ci.comm2 = &pr.comm;
		}
	}

	rv = key_handle(&ci);

	/* Just in case ... */
	PyErr_Clear();

	Py_XDECREF(s1); Py_XDECREF(s2);Py_XDECREF(s3);
	command_put(ci.comm2);
	if (ret && rv >= 0) {
		if (pr.ret)
			return pr.ret;
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (ret)
		Py_XDECREF(pr.ret);
	if (rv < Efalse) {
		PyErr_SetObject(Edlib_CommandFailed, PyLong_FromLong(rv));
		return NULL;
	}
	if (!return_char)
		return PyLong_FromLong(rv);
	if (!rv) {
		Py_INCREF(Py_False);
		return Py_False;
	}
	if (rv == CHAR_RET(WEOF)) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	return PyUnicode_FromFormat("%c", rv & 0xFFFFF);
}

static PyObject *pane_direct_call(Pane *self safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;

	if (!pane_valid(self))
		return NULL;

	ci.home = self->pane;

	rv = get_cmd_info(&ci, args, kwds, &s1, &s2);

	if (rv <= 0) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}

	ci.comm = ci.home->handle;
	rv = ci.comm->func(&ci);

	Py_XDECREF(s1); Py_XDECREF(s2);
	command_put(ci.comm2);
	if (rv < Efalse) {
		PyErr_SetObject(Edlib_CommandFailed, PyLong_FromLong(rv));
		return NULL;
	}
	return PyLong_FromLong(rv);
}

static PyObject *Pane_notify(Pane *self safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;

	if (!pane_valid(self))
		return NULL;

	ci.home = self->pane;

	rv = get_cmd_info(&ci, args, kwds, &s1, &s2);

	if (rv <= 0) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}

	rv = home_pane_notify(ci.home, ci.key, ci.focus, ci.num, ci.mark, ci.str,
			      ci.num2, ci.mark2, ci.str2,
			      ci.comm2);

	Py_XDECREF(s1); Py_XDECREF(s2);
	command_put(ci.comm2);
	if (rv < Efalse) {
		PyErr_SetObject(Edlib_CommandFailed, PyLong_FromLong(rv));
		return NULL;
	}
	return PyLong_FromLong(rv);
}

static PyObject *Pane_abs(Pane *self safe, PyObject *args)
{
	short x,y;
	short w= -1, h= -1;
	int have_height = 0;
	int ret = PyArg_ParseTuple(args, "hh|hh", &x, &y, &w, &h);
	if (ret <= 0)
		return NULL;
	if (h >= 0)
		have_height = 1;
	pane_absxy(self->pane, &x, &y, &w, &h);
	if (have_height)
		return Py_BuildValue("hhhh", x, y, w, h);
	else
		return Py_BuildValue("hh", x, y);
}

static PyObject *Pane_add_notify(Pane *self safe, PyObject *args)
{
	Pane *other = NULL;
	char *event = NULL;
	int ret = PyArg_ParseTuple(args, "O!s", &PaneType, &other, &event);
	if (ret <= 0 || !other || !event)
		return NULL;
	if (self->pane && other->pane)
		pane_add_notify(self->pane, other->pane, event);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_drop_notify(Pane *self safe, PyObject *args)
{
	char *event = NULL;
	int ret = PyArg_ParseTuple(args, "s", &event);
	if (ret <= 0 || !event)
		return NULL;
	if (self->pane)
		pane_drop_notifiers(self->pane, event);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_rel(Pane *self safe, PyObject *args)
{
	short x,y;
	int ret = PyArg_ParseTuple(args, "hh", &x, &y);
	if (ret <= 0)
		return NULL;
	pane_relxy(self->pane, &x, &y);
	return Py_BuildValue("ii", x, y);
}

static PyObject *Pane_damaged(Pane *self safe, PyObject *args)
{
	int damage = DAMAGED_CONTENT;
	int ret = PyArg_ParseTuple(args, "|i", &damage);
	if (ret <= 0)
		return NULL;
	if (self->pane)
		pane_damaged(self->pane, damage);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_close(Pane *self safe, PyObject *args)
{
	struct pane *p = self->pane;
	if (p) {
		pane_close(p);
		self->pane = NULL;
	}
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_get_scale(Pane *self safe, PyObject *args)
{
	struct pane *p = self->pane;
	struct xy xy = {1000, 1000};

	if (p)
		xy = pane_scale(p);
	return Py_BuildValue("ii", xy.x, xy.y);
}

static PyObject *Pane_mychild(Pane *self safe, PyObject *args)
{
	Pane *child = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &PaneType, &child);
	if (ret <= 0 || !child)
		return NULL;
	if (self->pane && child->pane) {
		struct pane *p = pane_my_child(self->pane, child->pane);
		if (p)
			return Pane_Frompane(p);
	}
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_clip(Pane *self safe, PyObject *args)
{
	Mark *start = NULL, *end = NULL;
	int view = -1;
	int ret = PyArg_ParseTuple(args, "i|O!O!", &view, &MarkType, &start,
				   &MarkType, &end);

	if (ret > 0 && start && end && self->pane &&
	    start->mark && end->mark && view >= 0)
		marks_clip(self->pane, start->mark, end->mark, view, self->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_reparent(Pane *self safe, PyObject *args)
{
	Pane *newparent = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &PaneType, &newparent);

	if (ret > 0 && newparent && self->pane && newparent->pane)
		pane_reparent(self->pane, newparent->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_move_after(Pane *self safe, PyObject *args)
{
	Pane *peer = NULL;
	int ret = PyArg_ParseTuple(args, "O", &peer);

	if (ret > 0 && peer && self->pane) {
		if ((PyObject*)peer == Py_None)
			pane_move_after(self->pane, NULL);
		else if (PyObject_TypeCheck(peer, &PaneType) && peer->pane)
			pane_move_after(self->pane, peer->pane);
	}
	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef pane_methods[] = {
	{"close", (PyCFunction)Pane_close, METH_NOARGS,
	 "close the pane"},
	{"children", (PyCFunction)pane_children, METH_NOARGS,
	 "provides an iterator which will iterate over all children"},
	{"clone_children", (PyCFunction)Pane_clone_children, METH_VARARGS,
	 "Clone all children onto the target"},
	{"take_focus", (PyCFunction)Pane_focus, METH_NOARGS,
	 "Claim the focus for this pane"},
	{"refresh", (PyCFunction)Pane_refresh, METH_NOARGS,
	 "Trigger refresh on this pane"},
	{"call", (void*)(PyCFunctionWithKeywords)Pane_call, METH_VARARGS|METH_KEYWORDS,
	 "Call a command from a pane"},
	{"notify", (void*)(PyCFunctionWithKeywords)Pane_notify, METH_VARARGS|METH_KEYWORDS,
	 "Send a notification from a pane"},
	{"abs", (PyCFunction)Pane_abs, METH_VARARGS,
	 "Convert pane-relative co-ords to absolute co-ords"},
	{"rel", (PyCFunction)Pane_rel, METH_VARARGS,
	 "Convert absolute co-orders to pane-relative"},
	{"add_notify", (PyCFunction)Pane_add_notify, METH_VARARGS,
	 "Add notifier for an event on some other pane"},
	{"drop_notify", (PyCFunction)Pane_drop_notify, METH_VARARGS,
	 "Drop notification to this pane for an event"},
	{"damaged", (PyCFunction)Pane_damaged, METH_VARARGS,
	 "Mark pane as damaged"},
	{"scale", (PyCFunction)Pane_get_scale, METH_NOARGS,
	 "Get the x,y scale numbers for this pane"},
	{"mychild", (PyCFunction)Pane_mychild, METH_VARARGS,
	 "Get ancestor of pane which is my child, or None"},
	{"clip", (PyCFunction)Pane_clip, METH_VARARGS,
	 "clip all 'type' marks in the given range"},
	{"reparent", (PyCFunction)Pane_reparent, METH_VARARGS,
	 "Give a pane a new parent"},
	{"move_after", (PyCFunction)Pane_move_after, METH_VARARGS,
	 "Move a pane after another in order of children"},
	{NULL}
};

static PyObject *pane_getnum(Pane *p safe, char *which safe)
{
	long n = 0;

	if (!pane_valid(p))
		return NULL;

	switch(*which) {
	case 'x': n = p->pane->x; break;
	case 'y': n = p->pane->y; break;
	case 'w': n = p->pane->w > 0 ? p->pane->w : 1; break;
	case 'h': n = p->pane->h > 0 ? p->pane->h : 1; break;
	case 'X': n = p->pane->cx; break;
	case 'Y': n = p->pane->cy; break;
	case 'z': n = p->pane->z; break;
	case 'Z': n = p->pane->abs_z; break;
	}
	return PyLong_FromLong(n);
}

static int pane_setnum(Pane *p safe, PyObject *v, char *which safe)
{
	int x,y,w,h;
	long val;

	if (!pane_valid(p))
		return -1;

	if (*which == 'z') {
		PyErr_SetString(PyExc_TypeError, "z cannot be set");
		return -1;
	}
	if (*which == 'Z') {
		PyErr_SetString(PyExc_TypeError, "abs_z cannot be set");
		return -1;
	}
	val = PyLong_AsLong(v);
	if (val == -1 && PyErr_Occurred())
		return -1;

	x = p->pane->x; y = p->pane->y;
	w = p->pane->w; h = p->pane->h;
	switch(*which) {
	case 'x': x = val; break;
	case 'y': y = val; break;
	case 'w': w = val; break;
	case 'h': h = val; break;
	case 'X': p->pane->cx = val; return 0;
	case 'Y': p->pane->cy = val; return 0;
	}
	pane_resize(p->pane, x, y, w, h);
	return 0;
}

static Pane *pane_getpane(Pane *p safe, char *which safe)
{
	struct pane *new = NULL;
	Pane *newpane;

	if (!pane_valid(p))
		return NULL;

	if (*which == 'p')
		new = p->pane->parent;
	if (*which == 'f')
		new = p->pane->focus;
	if (*which == 'r')
		new = pane_root(p->pane);
	if (new == NULL) {
		Py_INCREF(Py_None);
		newpane = (Pane*)Py_None;
	} else
		newpane = (Pane *)Pane_Frompane(new);

	return newpane;
}

static int pane_nosetpane(Pane *p, PyObject *v, void *which)
{
	PyErr_SetString(PyExc_TypeError, "Cannot set panes");
	return -1;
}

static PyObject *pane_repr(Pane *p safe)
{
	char buf[50];
	sprintf(buf, "<pane-0x%p>", p->pane);
	return Py_BuildValue("s", buf);
}

static PyObject *doc_repr(Pane *p safe)
{
	char buf[50];
	sprintf(buf, "<pane-0x%p>", p->pane);
	return Py_BuildValue("s", buf);
}

static long pane_hash(Pane *p safe)
{
	return (long)p->pane;
}

static PyObject *pane_cmp(Pane *p1 safe, Pane *p2 safe, int op)
{
	Py_RETURN_RICHCOMPARE(p1->pane, p2->pane, op);
}

static PyGetSetDef pane_getseters[] = {
    {"x",
     (getter)pane_getnum, (setter)pane_setnum,
     "X offset in parent", "x" },
    {"y",
     (getter)pane_getnum, (setter)pane_setnum,
     "Y offset in parent", "y" },
    {"z",
     (getter)pane_getnum, (setter)pane_setnum,
     "Z offset in parent", "z" },
    {"w",
     (getter)pane_getnum, (setter)pane_setnum,
     "width of pane", "w" },
    {"h",
     (getter)pane_getnum, (setter)pane_setnum,
     "heigth of pane", "h" },
    {"cx",
     (getter)pane_getnum, (setter)pane_setnum,
     "Cursor X offset in pane", "X" },
    {"cy",
     (getter)pane_getnum, (setter)pane_setnum,
     "Cursor Y offset in pane", "Y" },
    {"abs_z",
     (getter)pane_getnum, (setter)pane_setnum,
     "global Z offset", "Z" },
    {"parent",
     (getter)pane_getpane, (setter)pane_nosetpane,
     "Parent pane", "p"},
    {"focus",
     (getter)pane_getpane, (setter)pane_nosetpane,
     "Focal child", "f"},
    {"root",
     (getter)pane_getpane, (setter)pane_nosetpane,
     "Root pant", "r"},
    {NULL}  /* Sentinel */
};

static PyObject *Pane_get_item(Pane *self safe, PyObject *key safe)
{
	char *k, *v;
	PyObject *t1 = NULL;

	if (!pane_valid(self))
		return NULL;

	k = python_as_string(key, &t1);
	if (!k) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string or unicode");
		return NULL;
	}
	v = pane_attr_get(self->pane, k);
	Py_XDECREF(t1);
	if (v)
		return Py_BuildValue("s", v);
	Py_INCREF(Py_None);
	return Py_None;
}

static int Pane_set_item(Pane *self safe, PyObject *key, PyObject *val)
{
	char *k, *v;
	PyObject *t1 = NULL, *t2 = NULL;

	if (!pane_valid(self))
		return -1;

	k = python_as_string(key, &t1);
	if (!k) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string or unicode");
		return -1;
	}
	v = python_as_string(val, &t2);
	if (val != Py_None && !v) {
		PyErr_SetString(PyExc_TypeError, "value must be a string or unicode");
		Py_XDECREF(t1);
		return -1;
	}
	attr_set_str(&self->pane->attrs, k, v);
	Py_XDECREF(t1);
	Py_XDECREF(t2);
	return 0;
}

static PyMappingMethods pane_mapping = {
	.mp_length = NULL,
	.mp_subscript = (binaryfunc)Pane_get_item,
	.mp_ass_subscript = (objobjargproc)Pane_set_item,
};

static PyTypeObject PaneType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name	= "edlib.Pane",
	.tp_basicsize	= sizeof(Pane),
	.tp_dealloc	= (destructor)pane_dealloc,
	.tp_richcompare	= (richcmpfunc)pane_cmp,
	.tp_repr	= (reprfunc)pane_repr,
	.tp_as_mapping	= &pane_mapping,
	.tp_hash	= (hashfunc)pane_hash,
	.tp_call	= (ternaryfunc)pane_direct_call,
	.tp_flags	= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc		= "edlib panes",
	.tp_methods	= pane_methods,
	.tp_getset	= pane_getseters,
	.tp_init	= (initproc)Pane_init,
	.tp_new		= (newfunc)pane_new,
};

static PyTypeObject PaneIterType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name	= "edlib.PaneIter",
	.tp_basicsize	= sizeof(PaneIter),
	.tp_dealloc	= (destructor)paneiter_dealloc,
	.tp_flags	= Py_TPFLAGS_DEFAULT,
	.tp_doc		= "edlib pane iterator",
	.tp_iter	= (getiterfunc)pane_this_iter,
	.tp_iternext	= (iternextfunc)pane_iter_next,
	.tp_new		= (newfunc)pane_iter_new,
};

static PyObject *first_mark(Doc *self safe, PyObject *args)
{
	struct mark *m;

	if (!doc_valid(self))
		return NULL;

	m = doc_first_mark_all(&self->doc);
	if (!m) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	return Mark_Frommark(m);
}

static PyObject *to_end(Doc *self safe, PyObject *args)
{
	Mark *mark = NULL;
	int end = 0;
	int ret;

	if (!doc_valid(self))
		return NULL;

	ret = PyArg_ParseTuple(args, "O!i", &MarkType, &mark, &end);
	if (ret <= 0 || !mark || !mark->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark undefined or uninitialized");
		return NULL;
	}

	mark_to_end(&self->doc, mark->mark, end);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef doc_methods[] = {
	{"first_mark", (PyCFunction)first_mark, METH_NOARGS,
	 "first mark of document"},
	{"to_end", (PyCFunction)to_end, METH_VARARGS,
	 "Move mark to one end of document"},
	{NULL}
};

static PyTypeObject DocType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name	= "edlib.Doc",
	.tp_basicsize	= sizeof(Doc),
	.tp_dealloc	= (destructor)pane_dealloc,
	.tp_repr	= (reprfunc)doc_repr,
	.tp_flags	= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc		= "edlib document",
	.tp_methods	= doc_methods,
	.tp_base	= &PaneType,
	.tp_init	= (initproc)Doc_init,
	.tp_new		= (newfunc)Doc_new,
};

static PyObject *mark_getoffset(Mark *m safe, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (m->mark->owner->refcnt == mark_refcnt)
		return PyLong_FromLong(m->mark->ref.o);
	return PyLong_FromLong(0);
}

static int mark_setoffset(Mark *m safe, PyObject *v safe, void *x)
{
	long val;

	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	val = PyLong_AsLong(v);
	if (val == -1 && PyErr_Occurred())
		return -1;
	if (m->mark->owner->refcnt == mark_refcnt)
		m->mark->ref.o = val;
	else {
		PyErr_SetString(PyExc_TypeError, "Setting offset on non-local mark");
		return -1;
	}
	return 0;
}

static PyObject *mark_getseq(Mark *m safe, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	return PyLong_FromLong(m->mark->seq);
}

static int mark_nosetseq(Mark *m, PyObject *v, void *which)
{
	PyErr_SetString(PyExc_TypeError, "Cannot set mark seq number");
	return -1;
}

static void mark_refcnt(struct mark *m safe, int inc)
{
	if (!m->ref.c)
		return;
	while (inc > 0) {
		Py_INCREF(m->ref.c);
		inc -= 1;
	}
	while (inc < 0) {
		Py_DECREF(m->ref.c);
		inc += 1;
	}
}

static PyObject *mark_getpos(Mark *m safe, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (m->mark->owner->refcnt == mark_refcnt && m->mark->ref.c) {
		Py_INCREF(m->mark->ref.c);
		return m->mark->ref.c;
	} else {
		Py_INCREF(Py_None);
		return Py_None;
	}
}

static int mark_setpos(Mark *m safe, PyObject *v, void *x)
{
	struct mark *m2;
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	if (m->mark->owner->refcnt != mark_refcnt) {
		PyErr_SetString(PyExc_TypeError, "Not set ref for non-local mark");
		return -1;
	}
	m->mark->owner->refcnt(m->mark, -1);
	/* If an adjacent mark has a ref.c with a matching value
	 * use that instead, so that mark_same() works.
	 */
	m2 = doc_next_mark_all(m->mark);
	if (m2 && m2->owner->refcnt == mark_refcnt && m2->ref.c != NULL &&
	    PyObject_RichCompareBool(v, m2->ref.c, Py_EQ) == 1)
			m->mark->ref.c = m2->ref.c;
	else if ((m2 = doc_prev_mark_all(m->mark)) != NULL &&
		 m2->owner->refcnt == mark_refcnt &&
		 m2->ref.c != NULL &&
		 PyObject_RichCompareBool(v, m2->ref.c, Py_EQ) == 1)
			m->mark->ref.c = m2->ref.c;
	else
		m->mark->ref.c = v;
	m->mark->owner->refcnt(m->mark, 1);
	return 0;
}

static PyObject *mark_getview(Mark *m safe, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	return PyLong_FromLong(m->mark->viewnum);
}

static int mark_nosetview(Mark *m, PyObject *v, void *which)
{
	PyErr_SetString(PyExc_TypeError, "Cannot set mark viewnum");
	return -1;
}

static PyObject *mark_compare(Mark *a safe, Mark *b safe, int op)
{
	if ((PyObject*)a == Py_None)
		Py_RETURN_RICHCOMPARE(0, 1, op);
	else if ((PyObject*)b == Py_None)
		Py_RETURN_RICHCOMPARE(1, 0, op);
	else if (PyObject_TypeCheck(a, &MarkType) == 0 ||
		 PyObject_TypeCheck(b, &MarkType) == 0) {
		PyErr_SetString(PyExc_TypeError, "Mark compared with non-Mark");
		return NULL;
	} else if (!a->mark || !b->mark)
		return NULL;
	else {
		int cmp = a->mark->seq - b->mark->seq;
		if (mark_same(a->mark, b->mark))
			cmp = 0;
		Py_RETURN_RICHCOMPARE(cmp, 0, op);
	}
}

static PyGetSetDef mark_getseters[] = {
    {"pos",
     (getter)mark_getpos, (setter)mark_setpos,
     "Position ref", NULL},
    {"offset",
     (getter)mark_getoffset, (setter)mark_setoffset,
     "Position offset", NULL},
    {"viewnum",
     (getter)mark_getview, (setter)mark_nosetview,
     "Index for view list", NULL},
    {"seq",
     (getter)mark_getseq, (setter)mark_nosetseq,
     "Sequence number of mark", NULL},
    {NULL}  /* Sentinel */
};

static Mark *mark_new(PyTypeObject *type safe, PyObject *args, PyObject *kwds)
{
	Mark *self;

	self = (Mark *)type->tp_alloc(type, 0);
	if (self) {
		self->mark = NULL;
	}
	return self;
}

static int Mark_init(Mark *self safe, PyObject *args safe, PyObject *kwds)
{
	Pane *doc = NULL;
	Pane *owner = NULL;
	int view = MARK_UNGROUPED;
	Mark *orig = NULL;
	static char *keywords[] = {"pane","view","orig", "owner", NULL};
	int ret;

	if (!PyTuple_Check(args) ||
	    (PyTuple_GET_SIZE(args) == 0 && kwds == NULL))
		/* Internal Mark_Frommark call */
		return 1;

	ret = PyArg_ParseTupleAndKeywords(args, kwds, "|O!iO!O!", keywords,
					  &PaneType, &doc,
					  &view,
					  &MarkType, &orig,
					  &PaneType, &owner);
	if (ret <= 0)
		return -1;
	if (doc && orig) {
		PyErr_SetString(PyExc_TypeError,
				"Only one of 'pane' and 'orig' may be set");
		return -1;
	}
	if (!doc && !orig) {
		PyErr_SetString(PyExc_TypeError,
				"At least one of 'pane' and 'orig' must be set");
		return -1;
	}
	if (doc && doc->pane) {
		struct pane *p = doc->pane;
		struct pane *op = owner ? owner->pane : NULL;
		if (!op)
			op = p;
		self->mark = vmark_new(p, view, op);
	} else if (orig && orig->mark) {
		self->mark = mark_dup_view(orig->mark);
	}
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark creation failed");
		return -1;
	}
	if (self->mark->viewnum == MARK_POINT) {
		if (self->mark->owner->refcnt == mark_refcnt) {
			/* Python owns this point and can be expected to
			 * .release it when it is done
			 */
			self->mark->mtype = self;
			Py_INCREF(self);
		}
	} else if (self->mark->viewnum == MARK_UNGROUPED) {
		/* We want this mark to be freed when the Mark
		 * dies
		 */
		self->mark->mtype = self;
	} else {
		/* vmarks don't disappear until explicitly released */
		self->mark->mtype = &MarkType;
		self->mark->mdata = (PyObject*)self;
		Py_INCREF(self);
	}
	return 1;
}

static void mark_dealloc(Mark *self safe)
{
	if (self->mark) {
		if (self->mark->viewnum == MARK_POINT) {
			if (self->mark->owner->refcnt == mark_refcnt) {
				/* This has been released, so free it */
				mark_free(self->mark);
				self->mark = NULL;
			}
		} else if (self->mark->viewnum == MARK_UNGROUPED) {
			if (self->mark->mdata == (PyObject*)self) {
				/* I created, I can destroy it */
				struct mark *m = self->mark;
				self->mark = NULL;
				m->mdata = NULL;
				m->mtype = NULL;
				mark_free(m);
			}
		} else {
			if (self->mark->mtype == &MarkType) {
				struct mark *m = self->mark;
				ASSERT(m->mdata == (PyObject*)self);
				self->mark = NULL;
				m->mtype = NULL;
				m->mdata = NULL;
				mark_free(self->mark);
			}
		}
	}
	do_free((PyObject*safe)self);
}

static PyObject *Mark_to_mark(Mark *self safe, PyObject *args)
{
	Mark *other = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &MarkType, &other);
	if (ret <= 0 || !other || !self->mark || !other->mark)
		return NULL;
	mark_to_mark(self->mark, other->mark);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_to_mark_noref(Mark *self safe, PyObject *args)
{
	Mark *other = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &MarkType, &other);
	if (ret <= 0 || !other || !self->mark || !other->mark)
		return NULL;
	mark_to_mark_noref(self->mark, other->mark);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_clip(Mark *self safe, PyObject *args)
{
	Mark *start = NULL, *end = NULL;
	int ret = PyArg_ParseTuple(args, "O!O!", &MarkType, &start, &MarkType, &end);

	if (ret > 0 && start && end && self->mark &&
	    start->mark && end->mark)
		mark_clip(self->mark, start->mark, end->mark);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_next(Mark *self safe, PyObject *args)
{
	struct mark *next;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (self->mark->viewnum >= 0)
		next = vmark_next(self->mark);
	else
		next = NULL;
	if (next)
		return Mark_Frommark(next);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_prev(Mark *self safe, PyObject *args)
{
	struct mark *prev;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (self->mark->viewnum >= 0)
		prev = vmark_prev(self->mark);
	else
		prev = NULL;
	if (prev)
		return Mark_Frommark(prev);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_next_any(Mark *self safe, PyObject *args)
{
	struct mark *next;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	next = doc_next_mark_all(self->mark);
	if (next)
		return Mark_Frommark(next);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_prev_any(Mark *self safe, PyObject *args)
{
	struct mark *prev;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	prev = doc_prev_mark_all(self->mark);
	if (prev)
		return Mark_Frommark(prev);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_make_first(Mark *self safe, PyObject *args)
{
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	mark_make_first(self->mark);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_make_last(Mark *self safe, PyObject *args)
{
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	mark_make_last(self->mark);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_dup(Mark *self safe, PyObject *args)
{
	struct mark *new;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	new = mark_dup(self->mark);
	if (new) {
		Mark *ret = (Mark*)Mark_Frommark(new);
		/* We want this mark to be freed when the Mark
		 * dies
		 */
		new->mtype = ret;
		return (PyObject*)ret;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_release(Mark *self safe, PyObject *args)
{
	struct mark *m = self->mark;

	if (!m) {
		PyErr_SetString(PyExc_TypeError, "Mark has been freed");
		return NULL;
	}
	if (m->viewnum == MARK_UNGROUPED) {
		PyErr_SetString(PyExc_TypeError, "Cannot release ungrouped mark");
		return NULL;
	}
	if (m->viewnum == MARK_POINT && m->mtype == self) {
		/* We created this, time for it to go */
		m->mtype = NULL;
		Py_DECREF(self);
	} else if (m->viewnum >= 0 && m->mdata == (PyObject*)self) {
		/* We are dropping this mark - there cannot be any other ref */
		Py_DECREF(self);
		m->mdata = NULL;
		m->mtype = NULL;
		self->mark = NULL;
		mark_free(m);
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_ack(Mark *self safe, PyObject *args)
{
	struct mark *m = self->mark;

	if (!m) {
		PyErr_SetString(PyExc_TypeError, "Mark has been freed");
		return NULL;
	}
	mark_ack(m);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef mark_methods[] = {
	{"to_mark", (PyCFunction)Mark_to_mark, METH_VARARGS,
	 "Move one mark to another"},
	{"to_mark_noref", (PyCFunction)Mark_to_mark_noref, METH_VARARGS,
	 "Move one mark to another but don't update ref"},
	{"next", (PyCFunction)Mark_next, METH_NOARGS,
	 "next vmark"},
	{"prev", (PyCFunction)Mark_prev, METH_NOARGS,
	 "previous vmark"},
	{"next_any", (PyCFunction)Mark_next_any, METH_NOARGS,
	 "next any_mark"},
	{"prev_any", (PyCFunction)Mark_prev_any, METH_NOARGS,
	 "previous any_mark"},
	{"dup", (PyCFunction)Mark_dup, METH_NOARGS,
	 "duplicate a mark, as ungrouped"},
	{"clip", (PyCFunction)Mark_clip, METH_VARARGS,
	 "If this mark is in range, move to start"},
	{"release", (PyCFunction)Mark_release, METH_NOARGS,
	 "release a vmark so it can disappear"},
	{"ack", (PyCFunction)Mark_ack, METH_NOARGS,
	 "acknowledge movement of a point - allow further notifications"},
	{"make_first", (PyCFunction)Mark_make_first, METH_NOARGS,
		 "Make first of marks with same ref"},
	{"make_last", (PyCFunction)Mark_make_last, METH_NOARGS,
		 "Make last of marks with same ref"},
	{NULL}
};

static PyObject *mark_get_item(Mark *self safe, PyObject *key safe)
{
	char *k, *v;
	PyObject *t1 = NULL;

	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	k = python_as_string(key, &t1);
	if (!k) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string or unicode");
		return NULL;
	}
	v = attr_find(self->mark->attrs, k);
	Py_XDECREF(t1);
	if (v)
		return Py_BuildValue("s", v);
	Py_INCREF(Py_None);
	return Py_None;
}

static int mark_set_item(Mark *self safe, PyObject *key safe, PyObject *val safe)
{
	char *k, *v;
	PyObject *t1 = NULL, *t2 = NULL;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	k = python_as_string(key, &t1);
	if (!k) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string or unicode");
		return -1;
	}
	v = python_as_string(val, &t2);
	if (val != Py_None && !v) {
		PyErr_SetString(PyExc_TypeError, "value must be a string or unicode");
		Py_XDECREF(t1);
		return -1;
	}
	attr_set_str(&self->mark->attrs, k, v);
	Py_XDECREF(t1);
	Py_XDECREF(t2);
	return 0;
}

static PyMappingMethods mark_mapping = {
	.mp_length = NULL,
	.mp_subscript = (binaryfunc)mark_get_item,
	.mp_ass_subscript = (objobjargproc)mark_set_item,
};

static PyTypeObject MarkType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name	= "edlib.Mark",
	.tp_basicsize	= sizeof(Mark),
	.tp_dealloc	= (destructor)mark_dealloc,
	.tp_as_mapping	= &mark_mapping,
	.tp_flags	= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc		= "edlib marks",
	.tp_richcompare	= (richcmpfunc)mark_compare,
	.tp_methods	= mark_methods,
	.tp_getset	= mark_getseters,
	.tp_init	= (initproc)Mark_init,
	.tp_new		= (newfunc)mark_new,
};

static void comm_dealloc(Comm *self safe)
{
	command_put(self->comm);
	do_free((PyObject*safe)self);
}

static PyObject *comm_repr(Comm *p safe)
{
	char buf[50];
	if (!p->comm)
		return NULL;
	sprintf(buf, "<comm-0x%p/0x%p>", p->comm, p->comm->func);
	return Py_BuildValue("s", buf);
}

static PyObject *Comm_call(Comm *c safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;

	if (!c->comm)
		return NULL;
#if 0
	if (c->comm->func == python_call.func) {
		struct python_command *pc = container_of(c->comm, struct python_command,
							 c);
		return PyObject_Call(pc->callable, args, kwds);
	}
#endif
	rv = get_cmd_info(&ci, args, kwds, &s1, &s2);
	if (rv <= 0) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}
	ci.comm = c->comm;
	rv = c->comm->func(&ci);
	Py_XDECREF(s1); Py_XDECREF(s2);
	command_put(ci.comm2);

	if (rv < Efalse) {
		PyErr_SetObject(Edlib_CommandFailed, PyLong_FromLong(rv));
		return NULL;
	}
	return PyLong_FromLong(rv);
}

static Comm *comm_new(PyTypeObject *type safe, PyObject *args safe, PyObject *kwds)
{
	Comm *self;

	self = (Comm *)type->tp_alloc(type, 0);
	if (self)
		self->comm = NULL;
	return self;
}

static PyTypeObject CommType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name	= "edlib.Comm",
	.tp_basicsize	= sizeof(Comm),
	.tp_dealloc	= (destructor)comm_dealloc,
	.tp_repr	= (reprfunc)comm_repr,
	.tp_call	= (ternaryfunc)Comm_call,
	.tp_flags	= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc		= "edlib command",
	.tp_new		= (newfunc)comm_new,
};

static void python_free_command(struct command *c safe)
{
	struct python_command *pc = container_of(c, struct python_command, c);

	if (pc->callable)
		Py_DECREF(pc->callable);
	free(pc);
}

/* The 'call' function takes liberties with arg passing.
 * Positional args must start with the key, and then are handled based on
 * there type.  They can be panes, strigns, ints, pairs, marks, commands.
 * Panes are assigned to focus, then home.
 * Strings (after the key) are assigned to 'str' then 'str2'.

 * Ints are assigned to num then num2
 * Pairs (must be of ints) are assigned to x,y then hx,hy.
 * Marks are assigned to mark then mark2
 * A command is assigned to comm2 (comm is set automatically)
 * kwd arguments can also be used, currently
 * key, home, focus, xy, hxy, str, str2, mark, mark2, comm2.
 * A 'None' arg is ignored - probably a mark or string or something. Just use NULL
 */
static int get_cmd_info(struct cmd_info *ci safe, PyObject *args safe, PyObject *kwds,
			PyObject **s1 safe, PyObject **s2 safe)
{
	int argc;
	PyObject *a;
	int i;
	int num_set = 0, num2_set = 0;
	int xy_set = 0;

	*s1 = *s2 = NULL;

	if (!PyTuple_Check(args))
		return 0;
	argc = PyTuple_GET_SIZE(args);
	if (argc >= 1) {
		/* First positional arg must be the key */
		a = PyTuple_GetItem(args, 0);
		if (!PyUnicode_Check(a)) {
			PyErr_SetString(PyExc_TypeError, "First arg must be key");
			return 0;
		}
		ci->key = safe_cast PyUnicode_AsUTF8(a);
	}
	for (i = 1; i < argc; i++) {
		a = PyTuple_GetItem(args, i);
		if (a == Py_None)
			/* quietly ignore */;
		else if (PyObject_TypeCheck(a, &PaneType)) {
			if ((void*)ci->home == NULL)
				ci->home = safe_cast ((Pane*)a)->pane;
			else if ((void*)ci->focus == NULL)
				ci->focus = safe_cast ((Pane*)a)->pane;
			else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Pane args permitted");
				return 0;
			}
		} else if (PyObject_TypeCheck(a, &MarkType)) {
			if (ci->mark == NULL)
				ci->mark = ((Mark*)a)->mark;
			else if (ci->mark2 == NULL)
				ci->mark2 = ((Mark*)a)->mark;
			else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Mark args permitted");
				return 0;
			}
		} else if (PyUnicode_Check(a)) {
			char *str;
			PyObject *s = NULL;
			if (ci->str && ci->str2) {
				PyErr_SetString(PyExc_TypeError, "Only 3 String args permitted");
				return 0;
			}
			str = python_as_string(a, &s);
			if (!str)
				return 0;
			if (s) {
				*s2 = *s1;
				*s1 = s;
			}
			if (ci->str == NULL)
				ci->str = str;
			else
				ci->str2 = str;
		} else if (PyLong_Check(a)) {
			if (!num_set) {
				ci->num = PyLong_AsLong(a);
				num_set = 1;
			} else if (!num2_set) {
				ci->num2 = PyLong_AsLong(a);
				num2_set = 1;
			} else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Number args permitted");
				return 0;
			}
		} else if (PyTuple_Check(a)) {
			int n = PyTuple_GET_SIZE(a);
			PyObject *n1, *n2;
			if (n != 2) {
				PyErr_SetString(PyExc_TypeError, "Only 2-element tuples permitted");
				return 0;
			}
			n1 = PyTuple_GetItem(a, 0);
			n2 = PyTuple_GetItem(a, 1);
			if (!PyLong_Check(n1) || !PyLong_Check(n2)) {
				PyErr_SetString(PyExc_TypeError, "Only tuples of integers permitted");
				return 0;
			}
			if (!xy_set) {
				ci->x = PyLong_AsLong(n1);
				ci->y = PyLong_AsLong(n2);
				xy_set = 1;
			} else {
				PyErr_SetString(PyExc_TypeError, "Only one tuple permitted");
				return 0;
			}
		} else if (PyObject_TypeCheck(a, &CommType)) {
			Comm *c = (Comm*)a;
			if (ci->comm2 == NULL && c->comm) {
				ci->comm2 = command_get(c->comm);
			} else {
				PyErr_SetString(PyExc_TypeError, "Only one callable permitted");
				return 0;
			}
		} else if (PyCallable_Check(a)) {
			struct python_command *pc = malloc(sizeof(*pc));
			Py_INCREF(a);
			pc->callable = a;
			pc->c = python_call;
			pc->c.free = python_free_command;
			command_get(&pc->c);
			if (ci->comm2 == NULL)
				ci->comm2 = &pc->c;
			else {
				command_put(&pc->c);
				PyErr_SetString(PyExc_TypeError, "Only one callable permitted");
				return 0;
			}
		} else {
			PyErr_Format(PyExc_TypeError, "Unsupported arg type %d", i);
			return 0;
		}
	}
	if (kwds && PyDict_Check(kwds)) {
		a = PyDict_GetItemString(kwds, "str");
		if (a && a != Py_None) {
			if (*s1 || ci->str) {
				PyErr_SetString(PyExc_TypeError,
						"'str' given with other strings");
				return 0;
			}
			if (!PyUnicode_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'str' must be string or unicode");
				return 0;
			}
			ci->str = python_as_string(a, s1);
		}
		a = PyDict_GetItemString(kwds, "str2");
		if (a && a != Py_None) {
			if (*s2 || ci->str2) {
				PyErr_SetString(PyExc_TypeError,
						"'str2' given with 2 strings");
				return 0;
			}
			if (!PyUnicode_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'str2' must be string or unicode");
				return 0;
			}
			ci->str2 = python_as_string(a, s2);
		}
		a = PyDict_GetItemString(kwds, "mark");
		if (a && a != Py_None) {
			if (ci->mark) {
				PyErr_SetString(PyExc_TypeError,
						"'mark' given with other marks");
				return 0;
			}
			if (!PyObject_TypeCheck(a, &MarkType)) {
				PyErr_SetString(PyExc_TypeError,
						"'mark' must be an edlib.Mark");
				return 0;
			}
			ci->mark = ((Mark*)a)->mark;
		}
		a = PyDict_GetItemString(kwds, "mark2");
		if (a && a != Py_None) {
			if (ci->mark2) {
				PyErr_SetString(PyExc_TypeError,
						"'mark2' given with 2 other marks");
				return 0;
			}
			if (!PyObject_TypeCheck(a, &MarkType)) {
				PyErr_SetString(PyExc_TypeError,
						"'mark2' must be an edlib.Mark");
				return 0;
			}
			ci->mark2 = ((Mark*)a)->mark;
		}
		a = PyDict_GetItemString(kwds, "num");
		if (a) {
			if (num_set) {
				PyErr_SetString(PyExc_TypeError,
						"'num' given with other numbers");
				return 0;
			}
			if (!PyLong_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'num' must be an integer");
				return 0;
			}
			ci->num = PyLong_AsLong(a);
			num_set = 1;
		}
		a = PyDict_GetItemString(kwds, "num2");
		if (a) {
			if (num2_set) {
				PyErr_SetString(PyExc_TypeError,
						"'num2' given with 2 other numbers");
				return 0;
			}
			if (!PyLong_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'num2' must be an integer");
				return 0;
			}
			ci->num2 = PyLong_AsLong(a);
			num2_set = 1;
		}
	}
	if (!(void*)ci->key) {
		PyErr_SetString(PyExc_TypeError, "No key specified");
		return 0;
	}
	if (!(void*)ci->home) {
		PyErr_SetString(PyExc_TypeError, "No pane specified");
		return 0;
	}
	if (!(void*)ci->focus)
		ci->focus = ci->home;

	return xy_set ? 2 : 1;
}

static PyObject *py_time_start(PyObject *self, PyObject *args)
{
	int type;
	int ret = PyArg_ParseTuple(args, "i", &type);

	if (ret <= 0)
		return NULL;
	time_start(type);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *py_time_stop(PyObject *self, PyObject *args)
{
	int type;
	int ret = PyArg_ParseTuple(args, "i", &type);

	if (ret <= 0)
		return NULL;
	time_stop(type);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef edlib_methods[] = {
	{"time_start", py_time_start, METH_VARARGS,
	 "Record start time"},
	{"time_stop", py_time_stop, METH_VARARGS,
	 "Record stop time"},
	{NULL, NULL, 0, NULL}
};

extern char *edlib_module_path;
char *edlib_module_path;

static struct PyModuleDef edlib_mod = {
	PyModuleDef_HEAD_INIT,
	.m_name		= "edlib",
	.m_doc		= "edlib - one more editor is never enough.",
	.m_methods	= edlib_methods,
};

void edlib_init(struct pane *ed safe)
{
	PyObject *m;
	wchar_t *argv[1]= { NULL };

	if (edlib_module_path)
		module_dir = strdup(edlib_module_path);
	else
		module_dir = ".";

	/* This cast is for sparse, which doesn't seem to cope with L".."
	 * FIXME
	 */
	Py_SetProgramName((wchar_t*)L"edlib");
	Py_Initialize();
	PySys_SetArgv(0, argv);

	PaneType.tp_new = PyType_GenericNew;
	PaneIterType.tp_new = PyType_GenericNew;
	DocType.tp_new = PyType_GenericNew;
	MarkType.tp_new = PyType_GenericNew;
	CommType.tp_new = PyType_GenericNew;
	if (PyType_Ready(&PaneType) < 0 ||
	    PyType_Ready(&PaneIterType) < 0 ||
	    PyType_Ready(&DocType) < 0 ||
	    PyType_Ready(&MarkType) < 0 ||
	    PyType_Ready(&CommType) < 0)
		return;

	m = PyModule_Create(&edlib_mod);

	if (!m)
		return;

	PyModule_AddObject(m, "Pane", (PyObject *)&PaneType);
	PyModule_AddObject(m, "PaneIter", (PyObject *)&PaneIterType);
	PyModule_AddObject(m, "Mark", (PyObject *)&MarkType);
	PyModule_AddObject(m, "Comm", (PyObject *)&CommType);
	PyModule_AddObject(m, "Doc", (PyObject *)&DocType);
	PyModule_AddIntMacro(m, DAMAGED_CHILD);
	PyModule_AddIntMacro(m, DAMAGED_SIZE);
	PyModule_AddIntMacro(m, DAMAGED_VIEW);
	PyModule_AddIntMacro(m, DAMAGED_CONTENT);
	PyModule_AddIntMacro(m, DAMAGED_CURSOR);
	PyModule_AddIntMacro(m, DAMAGED_POSTORDER);
	PyModule_AddIntMacro(m, DAMAGED_CLOSED);
	PyModule_AddIntMacro(m, Efallthrough);
	PyModule_AddIntMacro(m, Enoarg);
	PyModule_AddIntMacro(m, Einval);
	PyModule_AddIntMacro(m, Efalse);
	PyModule_AddIntMacro(m, Efail);
	PyModule_AddIntMacro(m, Enosup);
	PyModule_AddIntMacro(m, Efail);
	PyModule_AddIntMacro(m, NO_NUMERIC);

	PyModule_AddIntMacro(m, TIME_KEY);
	PyModule_AddIntMacro(m, TIME_WINDOW);
	PyModule_AddIntMacro(m, TIME_READ);
	PyModule_AddIntMacro(m, TIME_SIG);
	PyModule_AddIntMacro(m, TIME_TIMER);
	PyModule_AddIntMacro(m, TIME_IDLE);
	PyModule_AddIntMacro(m, TIME_REFRESH);

	PyModule_AddIntConstant(m, "WEOF", 0x1FFFFF);
	call_comm("global-set-command", ed, &python_load, 0, NULL, "python-load");
	call_comm("global-set-command", ed, &python_load_module,
		  0, NULL, "global-load-modules:python");

	Edlib_CommandFailed = PyErr_NewException("edlib.commandfailed", NULL, NULL);
	Py_INCREF(Edlib_CommandFailed);
	PyModule_AddObject(m, "commandfailed", Edlib_CommandFailed);
	EdlibModule = m;
}
