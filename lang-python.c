/*
 * Copyright Neil Brown ©2015 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Python bindings for edlib.
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
 *                get/set operations for rpos.
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

static PyObject *Edlib_CommandFailed;
static PyObject *EdlibModule;

/* When a python callable is passed to edlib_call() we combine it
 * with this "python_call" for edlib to call back that callable.
 */
struct python_command {
	struct command	c;
	PyObject	*callable;
};
DEF_CMD(python_call);
DEF_CMD(python_doc_call);

typedef struct {
	PyObject_HEAD
	struct pane	*pane;
	struct python_command handle;
} Pane;
static PyTypeObject PaneType;

typedef struct {
	PyObject_HEAD
	struct pane	*pane;
	struct python_command handle;
	struct doc	doc;
} Doc;
static PyTypeObject DocType;

typedef struct {
	PyObject_HEAD
	struct mark	*mark;
	int		released;
	int		local; /* set when mark arrived with ci->home being
				* a Doc, so the ref is usable
				*/
} Mark;
static PyTypeObject MarkType;

typedef struct {
	PyObject_HEAD
	struct command	*comm;
} Comm;
static PyTypeObject CommType;

static int get_cmd_info(struct cmd_info *ci, PyObject *args, PyObject *kwds);

static inline PyObject *Pane_Frompane(struct pane *p)
{
	Pane *pane;
	if (p && p->handle && p->handle->func == python_call.func) {
		pane = p->data;
		Py_INCREF(pane);
	} else if (p && p->handle && p->handle->func == python_doc_call.func) {
		struct doc *doc = p->data;
		Doc *pdoc = container_of(doc, Doc, doc);
		pane = (Pane*)pdoc;
		Py_INCREF(pane);
	} else {
		pane = (Pane *)PyObject_CallObject((PyObject*)&PaneType, NULL);
		pane->pane = p;
	}
	return (PyObject*)pane;
}

static inline PyObject *Mark_Frommark(struct mark *m, int local)
{
	Mark *mark;

	if (m->mtype == &MarkType) {
		Py_INCREF(m->mdata);
		return m->mdata;
	}
	mark = (Mark *)PyObject_CallObject((PyObject*)&MarkType, NULL);
	mark->mark = m;
	mark->released = 1;
	mark->local = local;
	return (PyObject*)mark;
}

static inline PyObject *Comm_Fromcomm(struct command *c)
{
	if (c->func == python_call_func && 0) {
		struct python_command *pc = container_of(c, struct python_command, c);
		Py_INCREF(pc->callable);
		return pc->callable;
	} else {
		Comm *comm = (Comm*)PyObject_CallObject((PyObject*)&CommType, NULL);
		comm->comm = c;
		return (PyObject*)comm;
	}
}

DEF_CMD(python_load)
{
	char *fname = ci->str;
	FILE *fp;
	PyObject *globals, *main_mod;
	PyObject *Ed;

	if (!fname)
		return -1;
	fp = fopen(fname, "r");
	if (!fp)
		return -1;

	main_mod = PyImport_AddModule("__main__");
	if (main_mod == NULL)
		return -1;
	globals = PyModule_GetDict(main_mod);

	Ed = Pane_Frompane(ci->home);
	PyDict_SetItemString(globals, "editor", Ed);
	PyDict_SetItemString(globals, "pane", Pane_Frompane(ci->focus));
	PyDict_SetItemString(globals, "edlib", EdlibModule);
	PyRun_FileExFlags(fp, fname, Py_file_input, globals, globals, 0, NULL);
	PyErr_Print();
	Py_DECREF(Ed);
	fclose(fp);
	return 1;
}

DEF_CMD(python_load_module)
{
	char *name = ci->str;
	FILE *fp;
	PyObject *globals, *main_mod;
	PyObject *Ed;
	char buf [PATH_MAX];

	if (!name)
		return -1;
	snprintf(buf, sizeof(buf), "python/%s.py", name);
	fp = fopen(buf, "r");
	if (!fp)
		return -1;

	main_mod = PyImport_AddModule("__main__");
	if (main_mod == NULL)
		return -1;
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

REDEF_CMD(python_call)
{
	struct python_command *pc = container_of(ci->comm, struct python_command, c);
	PyObject *ret, *args, *kwds;
	int rv = 1;
	int local;

	args = Py_BuildValue("(s)", ci->key);
	kwds = PyDict_New();
	PyDict_SetItemString(kwds, "home", Pane_Frompane(ci->home));
	local = ci->home && ci->home->handle &&
		ci->home->handle->func == python_doc_call.func;
	if (ci->focus)
		PyDict_SetItemString(kwds, "focus", Pane_Frompane(ci->focus));
	if (ci->mark)
		PyDict_SetItemString(kwds, "mark", Mark_Frommark(ci->mark, local));
	if (ci->mark2)
		PyDict_SetItemString(kwds, "mark2", Mark_Frommark(ci->mark2, local));
	if (ci->str)
		PyDict_SetItemString(kwds, "str", Py_BuildValue("s", ci->str));
	if (ci->str2)
		PyDict_SetItemString(kwds, "str2", Py_BuildValue("s", ci->str2));
	if (ci->comm)
		PyDict_SetItemString(kwds, "comm", Comm_Fromcomm(ci->comm));
	if (ci->comm2)
		PyDict_SetItemString(kwds, "comm2", Comm_Fromcomm(ci->comm2));
	PyDict_SetItemString(kwds, "numeric", Py_BuildValue("i", ci->numeric));
	PyDict_SetItemString(kwds, "extra", Py_BuildValue("i", ci->extra));
	PyDict_SetItemString(kwds, "xy", Py_BuildValue("ii", ci->x, ci->y));

	ret = PyObject_Call(pc->callable, args, kwds);

	Py_DECREF(args);
	Py_DECREF(kwds);
	if (!ret) {
		PyErr_Print();
		/* FIXME cancel error?? */
		return -1;
	}
	if (ret == Py_None)
		rv = 0;
	else if (PyInt_Check(ret))
		rv = PyInt_AsLong(ret);
	else
		rv = 1;
	Py_DECREF(ret);
	return rv;
}

REDEF_CMD(python_doc_call)
{
	int rv = python_call_func(ci);
	if (rv == 0)
		rv = key_lookup(doc_default_cmd, ci);
	return rv;
}

static Pane *pane_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	Pane *self;

	self = (Pane *)type->tp_alloc(type, 0);
	if (self) {
		self->pane = NULL;
	}
	return self;
}

static void doc_free(struct doc *d)
{
	/* A bit like calling .release() on the pane */
	Doc *pd = container_of(d, Doc, doc);
	struct pane *p = pd->pane;
	if (p) {
		p->handle = NULL;
		p->data = NULL;
	}
	Py_DECREF(pd);
}

static Doc *Doc_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	Doc *self;

	self = (Doc *)type->tp_alloc(type, 0);
	if (self) {
		self->pane = NULL;
		doc_init(&self->doc);
		self->doc.free = doc_free;
	}
	return self;
}

static int __Pane_init(Pane *self, PyObject *args, PyObject *kwds, Pane **parentp,
		       int *zp)
{
	Pane *parent;
	PyObject *py_handler;
	int ret;
	static char *keywords[] = {"parent", "handler", "z", NULL};

	if (self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane already initialised");
		return -1;
	}
	/* Pane(parent, handler, data, z=0 */
	if (!PyTuple_Check(args) || PyTuple_GET_SIZE(args) == 0)
		/* Probably an internal Pane_Frompane call */
		return 0;

	ret = PyArg_ParseTupleAndKeywords(args, kwds, "O!O|i", keywords,
					  &PaneType, &parent, &py_handler,
					  zp);
	if (ret <= 0)
		return -1;
	if (!PyCallable_Check(py_handler)) {
		PyErr_SetString(PyExc_TypeError, "'handler' is not callable");
		return -1;
	}
	self->handle.c = python_call;
	Py_INCREF(py_handler);
	self->handle.callable = py_handler;
	*parentp = parent;
	return 1;
}

static int Pane_init(Pane *self, PyObject *args, PyObject *kwds)
{
	Pane *parent = NULL;
	int z = 0;
	int ret = __Pane_init(self, args, kwds, &parent, &z);

	if (ret < 0 || !parent)
		return ret;

	self->pane = pane_register(parent->pane, z, &self->handle.c, self, NULL);
	return 0;
}

static int Doc_init(Doc *self, PyObject *args, PyObject *kwds)
{
	Pane *parent = NULL;
	int z = 0;
	int ret = __Pane_init((Pane*)self, args, kwds, &parent, &z);

	if (ret <= 0 || !parent)
		return ret;

	self->handle.c = python_doc_call;
	doc_init(&self->doc);
	self->pane = pane_register(parent->pane, z, &self->handle.c, &self->doc, NULL);
	self->doc.home = self->pane;
	return 0;
}

static void pane_dealloc(Pane *self)
{
	self->ob_type->tp_free((PyObject*)self);
}

static Pane *pane_children(Pane *self)
{
	if (!self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane is NULL");
		return NULL;
	}
	if (list_empty(&self->pane->children)) {
		/* FIXME is this right for an empty iterator? */
		Py_INCREF(Py_None);
		return (Pane*)Py_None;
	}

	return (Pane*)Pane_Frompane(list_first_entry(&self->pane->children,
						     struct pane, siblings));
}

static PyObject *Pane_clone_children(Pane *self, PyObject *args)
{
	Pane *other = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &PaneType, &other);

	if (ret <= 0)
		return NULL;
	if (self->pane && other->pane)
		pane_clone_children(self->pane, other->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_focus(Pane *self)
{
	if (self->pane)
		pane_focus(self->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_refresh(Pane *self)
{
	if (self->pane)
		pane_refresh(self->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static Pane *pane_this(Pane *self)
{
	return (Pane *)Pane_Frompane(self->pane);
}

static Pane *pane_next(Pane *self)
{
	if (!self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane is NULL");
		return NULL;
	}
	if (self->pane->siblings.next == &self->pane->parent->children) {
		/* Reached the end of the list */
		return NULL;
	}

	return (Pane*)Pane_Frompane(list_next_entry(self->pane, siblings));
}

static PyObject *Pane_call(Pane *self, PyObject *args, PyObject *kwds)
{
	struct cmd_info ci = {};
	int rv;

	ci.home = self->pane;

	rv = get_cmd_info(&ci, args, kwds);

	if (rv <= 0)
		return NULL;

	rv = key_handle(&ci);

	if (!rv) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (rv < 0) {
		PyErr_SetObject(Edlib_CommandFailed, PyInt_FromLong(rv));
		return NULL;
	}
	return PyInt_FromLong(rv);
}

static PyObject *Pane_notify(Pane *self, PyObject *args, PyObject *kwds)
{
	struct cmd_info ci = {};
	int rv;

	ci.home = self->pane;

	rv = get_cmd_info(&ci, args, kwds);

	if (rv <= 0)
		return NULL;

	rv = pane_notify(ci.focus, ci.key, ci.mark, ci.mark2, ci.str,
			 ci.numeric, ci.comm2);

	if (!rv) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (rv < 0) {
		PyErr_SetObject(Edlib_CommandFailed, PyInt_FromLong(rv));
		return NULL;
	}
	return PyInt_FromLong(rv);
}

static PyObject *Pane_abs(Pane *self, PyObject *args)
{
	int x,y;
	int ret = PyArg_ParseTuple(args, "ii", &x, &y);
	if (ret <= 0)
		return NULL;
	pane_absxy(self->pane, &x, &y);
	return Py_BuildValue("ii", x, y);
}

static PyObject *Pane_add_notify(Pane *self, PyObject *args)
{
	Pane *other = NULL;
	char *event = NULL;
	int ret = PyArg_ParseTuple(args, "O!s", &PaneType, &other, &event);
	if (ret <= 0)
		return NULL;
	pane_add_notify(self->pane, other->pane, event);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_rel(Pane *self, PyObject *args)
{
	int x,y;
	int ret = PyArg_ParseTuple(args, "ii", &x, &y);
	if (ret <= 0)
		return NULL;
	pane_relxy(self->pane, &x, &y);
	return Py_BuildValue("ii", x, y);
}

static PyObject *Pane_render_attach(Pane *self, PyObject *args)
{
	char *type = NULL;
	struct pane *p;
	int ret = PyArg_ParseTuple(args, "s", &type);
	if (ret <= 0)
		return NULL;
	p = render_attach(type, self->pane);
	if (!p) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	return Pane_Frompane(p);
}

static PyObject *Pane_damaged(Pane *self, PyObject *args)
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

static PyObject *Pane_close(Pane *self)
{
	struct pane *p = self->pane;
	if (p) {
		pane_close(p);
		self->pane = NULL;
	}
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_release(Pane *self)
{
	struct pane *p = self->pane;
	if (p && p->handle && p->handle->func == python_call.func && p->data) {
		p->handle = NULL;
		p->data = NULL;
		Py_DECREF(self);
	}
	if (p && p->handle && p->handle->func == python_doc_call.func && p->data) {
		p->handle = NULL;
		p->data = NULL;
		Py_DECREF(self);
	}
	self->pane = NULL;
	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef pane_methods[] = {
	{"close", (PyCFunction)Pane_close, METH_NOARGS,
	 "close the pane"},
	{"release", (PyCFunction)Pane_release, METH_NOARGS,
	 "pane is being closed, so release the Pane"},
	{"children", (PyCFunction)pane_children, METH_NOARGS,
	 "provides and iterator which will iterate over all children"},
	{"clone_children", (PyCFunction)Pane_clone_children, METH_VARARGS,
	 "Clone all children onto the target"},
	{"take_focus", (PyCFunction)Pane_focus, METH_NOARGS,
	 "Claim the focus for this pane"},
	{"refresh", (PyCFunction)Pane_refresh, METH_NOARGS,
	 "Trigger refresh on this pane"},
	{"call", (PyCFunction)Pane_call, METH_VARARGS|METH_KEYWORDS,
	 "Call a command from a pane"},
	{"notify", (PyCFunction)Pane_notify, METH_VARARGS|METH_KEYWORDS,
	 "Send a notification from a pane"},
	{"abs", (PyCFunction)Pane_abs, METH_VARARGS,
	 "Convert pane-relative co-ords to absolute co-ords"},
	{"rel", (PyCFunction)Pane_rel, METH_VARARGS,
	 "Convert absolute co-orders to pane-relative"},
	{"add_notify", (PyCFunction)Pane_add_notify, METH_VARARGS,
	 "Add notified for an event on some other pane"},
	{"render_attach", (PyCFunction)Pane_render_attach, METH_VARARGS,
	 "Attach a renderer to a pane"},
	{"damaged", (PyCFunction)Pane_damaged, METH_VARARGS,
	 "Mark pane as damaged"},
	{NULL}
};

static PyObject *pane_getnum(Pane *p, char *which)
{
	long n = 0;
	if (p->pane == NULL) {
		PyErr_SetString(PyExc_TypeError, "Pane is NULL");
		return NULL;
	}
	switch(*which) {
	case 'x': n = p->pane->x; break;
	case 'y': n = p->pane->y; break;
	case 'w': n = p->pane->w; break;
	case 'h': n = p->pane->h; break;
	case 'X': n = p->pane->cx; break;
	case 'Y': n = p->pane->cy; break;
	case 'z': n = p->pane->z; break;
	case 'Z': n = p->pane->abs_z; break;
	}
	return PyInt_FromLong(n);
}

static int pane_setnum(Pane *p, PyObject *v, char *which)
{
	int x,y,w,h;
	long val;

	if (p->pane == NULL) {
		PyErr_SetString(PyExc_TypeError, "Pane is NULL");
		return -1;
	}
	if (*which == 'z') {
		PyErr_SetString(PyExc_TypeError, "z cannot be set");
		return -1;
	}
	if (*which == 'Z') {
		PyErr_SetString(PyExc_TypeError, "abs_z cannot be set");
		return -1;
	}
	val = PyInt_AsLong(v);
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

static Pane *pane_getpane(Pane *p, char *which)
{
	struct pane *new = NULL;
	Pane *newpane;

	if (p->pane == NULL) {
		PyErr_SetString(PyExc_TypeError, "pane not initialized");
		return NULL;
	}
	if (*which == 'p')
		new = p->pane->parent;
	if (*which == 'f')
		new = p->pane->focus;
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

static PyObject *pane_repr(Pane *p)
{
	char buf[50];
	sprintf(buf, "<pane-0x%p>", p->pane);
	return Py_BuildValue("s", buf);
}

static long pane_hash(Pane *p)
{
	return (long)p->pane;
}

static long pane_cmp(Pane *p1, Pane *p2)
{
	if (p1->pane == p2->pane)
		return 0;
	if (p1->pane < p2->pane)
		return -1;
	return 1;
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
    {NULL}  /* Sentinel */
};

static PyObject *Pane_get_item(Pane *self, PyObject *key)
{
	char *k, *v;
	if (!self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane is NULL");
		return NULL;
	}
	if (!PyString_Check(key)) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string");
		return NULL;
	}
	k = PyString_AsString(key);
	v = pane_attr_get(self->pane, k);
	if (v)
		return Py_BuildValue("s", v);
	Py_INCREF(Py_None);
	return Py_None;
}

static int Pane_set_item(Pane *self, PyObject *key, PyObject *val)
{
	char *k, *v;
	if (!self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane is NULL");
		return -1;
	}
	if (!PyString_Check(key)) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string");
		return -1;
	}
	if (!PyString_Check(val)) {
		PyErr_SetString(PyExc_TypeError, "value must be a string");
		return -1;
	}
	k = PyString_AsString(key);
	v = PyString_AsString(val);
	attr_set_str(&self->pane->attrs, k, v);
	return 0;
}

static PyMappingMethods pane_mapping = {
	.mp_length = NULL,
	.mp_subscript = (binaryfunc)Pane_get_item,
	.mp_ass_subscript = (objobjargproc)Pane_set_item,
};

static PyTypeObject PaneType = {
    PyObject_HEAD_INIT(NULL)
    0,				/*ob_size*/
    "edlib.Pane",		/*tp_name*/
    sizeof(Pane),		/*tp_basicsize*/
    0,				/*tp_itemsize*/
    (destructor)pane_dealloc,	/*tp_dealloc*/
    NULL,			/*tp_print*/
    NULL,			/*tp_getattr*/
    NULL,			/*tp_setattr*/
    (cmpfunc)pane_cmp,		/*tp_compare*/
    (reprfunc)pane_repr,	/*tp_repr*/
    NULL,			/*tp_as_number*/
    NULL,			/*tp_as_sequence*/
    &pane_mapping,		/*tp_as_mapping*/
    (hashfunc)pane_hash,	/*tp_hash */
    NULL,			/*tp_call*/
    NULL,			/*tp_str*/
    NULL,			/*tp_getattro*/
    NULL,			/*tp_setattro*/
    NULL,			/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "edlib panes",		/* tp_doc */
    NULL,			/* tp_traverse */
    NULL,			/* tp_clear */
    NULL,			/* tp_richcompare */
    0,				/* tp_weaklistoffset */
    (getiterfunc)pane_this,	/* tp_iter */
    (iternextfunc)pane_next,	/* tp_iternext */
    pane_methods,		/* tp_methods */
    NULL,			/* tp_members */
    pane_getseters,		/* tp_getset */
    NULL,			/* tp_base */
    NULL,			/* tp_dict */
    NULL,			/* tp_descr_get */
    NULL,			/* tp_descr_set */
    0,				/* tp_dictoffset */
    .tp_init = (initproc)Pane_init,/* tp_init */
    NULL,			/* tp_alloc */
    .tp_new = (newfunc)pane_new,/* tp_new */
};

static PyTypeObject DocType = {
    PyObject_HEAD_INIT(NULL)
    0,				/*ob_size*/
    "edlib.Doc",		/*tp_name*/
    sizeof(Doc),		/*tp_basicsize*/
    0,				/*tp_itemsize*/
    (destructor)pane_dealloc,	/*tp_dealloc*/
    NULL,			/*tp_print*/
    NULL,			/*tp_getattr*/
    NULL,			/*tp_setattr*/
    (cmpfunc)pane_cmp,		/*tp_compare*/
    (reprfunc)pane_repr,	/*tp_repr*/
    NULL,			/*tp_as_number*/
    NULL,			/*tp_as_sequence*/
    &pane_mapping,		/*tp_as_mapping*/
    (hashfunc)pane_hash,	/*tp_hash */
    NULL,			/*tp_call*/
    NULL,			/*tp_str*/
    NULL,			/*tp_getattro*/
    NULL,			/*tp_setattro*/
    NULL,			/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "edlib document",		/* tp_doc */
    NULL,			/* tp_traverse */
    NULL,			/* tp_clear */
    NULL,			/* tp_richcompare */
    0,				/* tp_weaklistoffset */
    (getiterfunc)pane_this,	/* tp_iter */
    (iternextfunc)pane_next,	/* tp_iternext */
    pane_methods,		/* tp_methods */
    NULL,			/* tp_members */
    pane_getseters,		/* tp_getset */
    &PaneType,			/* tp_base */
    NULL,			/* tp_dict */
    NULL,			/* tp_descr_get */
    NULL,			/* tp_descr_set */
    0,				/* tp_dictoffset */
    .tp_init = (initproc)Doc_init,/* tp_init */
    NULL,			/* tp_alloc */
    .tp_new = (newfunc)Doc_new,/* tp_new */
};

static PyObject *mark_getrpos(Mark *m, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (x == NULL)
		return PyInt_FromLong(m->mark->rpos);
	if (m->local)
		return PyInt_FromLong(m->mark->ref.o);
	return PyInt_FromLong(0);
}

static int mark_setrpos(Mark *m, PyObject *v, void *x)
{
	long val;

	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	val = PyInt_AsLong(v);
	if (val == -1 && PyErr_Occurred())
		return -1;
	if (x == NULL)
		m->mark->rpos = val;
	else if (m->local)
		m->mark->ref.o = val;
	else {
		PyErr_SetString(PyExc_TypeError, "Setting offset on non-local mark");
		return -1;
	}
	return 0;
}

static PyObject *mark_getpos(Mark *m, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (m->local && m->mark->ref.c) {
		Py_INCREF(m->mark->ref.c);
		return m->mark->ref.c;
	} else {
		Py_INCREF(Py_None);
		return Py_None;
	}
}

static int mark_setpos(Mark *m, PyObject *v, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	if (!m->local) {
		PyErr_SetString(PyExc_TypeError, "Not set ref for non-local mark");
		return -1;
	}
	if (m->mark->ref.c)
		Py_DECREF(m->mark->ref.c);
	m->mark->ref.c = v;
	Py_INCREF(v);
	return 0;
}


static PyObject *mark_getview(Mark *m, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	return PyInt_FromLong(m->mark->viewnum);
}

static int mark_nosetview(Mark *m, PyObject *v, void *which)
{
	PyErr_SetString(PyExc_TypeError, "Cannot set mark viewnum");
	return -1;
}

static PyObject *mark_compare(Mark *a, Mark *b, int op)
{
	int ret = 0;
	PyObject *rv;

	if ((PyObject*)a == Py_None)
		ret = (op == Py_LT || op == Py_LE || op == Py_NE);
	else if ((PyObject*)b == Py_None)
		ret = (op == Py_GT || op == Py_GE || op == Py_EQ);
	else if (PyObject_TypeCheck(a, &MarkType) == 0 ||
		 PyObject_TypeCheck(b, &MarkType) == 0) {
		PyErr_SetString(PyExc_TypeError, "Mark compared with non-Mark");
		return NULL;
	} else {
		int cmp = a->mark->seq - b->mark->seq;
		switch(op) {
		case Py_LT: ret = cmp <  0; break;
		case Py_LE: ret = cmp <= 0; break;
		case Py_GT: ret = cmp >  0; break;
		case Py_GE: ret = cmp >= 0; break;
		case Py_EQ: ret = cmp == 0; break;
		case Py_NE: ret = cmp != 0; break;
		}
	}
	rv = ret ? Py_True : Py_False;
	Py_INCREF(rv);
	return rv;
}

static PyGetSetDef mark_getseters[] = {
    {"pos",
     (getter)mark_getpos, (setter)mark_setpos,
     "Position ref", NULL},
    {"offset",
     (getter)mark_getrpos, (setter)mark_setrpos,
     "Position offset", (void*)1},
    {"rpos",
     (getter)mark_getrpos, (setter)mark_setrpos,
     "Rendering Position",  NULL},
    {"viewnum",
     (getter)mark_getview, (setter)mark_nosetview,
     "Index for view list", NULL},
    {NULL}  /* Sentinel */
};

static Mark *mark_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	Mark *self;

	self = (Mark *)type->tp_alloc(type, 0);
	if (self) {
		self->mark = NULL;
	}
	return self;
}

static int Mark_init(Mark *self, PyObject *args, PyObject *kwds)
{
	Pane *doc = NULL;
	int view = MARK_UNGROUPED;
	Mark *orig = NULL;
	static char *keywords[] = {"pane","view","orig", NULL};
	int ret;
	int local;

	if (!PyTuple_Check(args) ||
	    (PyTuple_GET_SIZE(args) == 0 && kwds == NULL))
		/* Internal Mark_Frommark call */
		return 1;

	ret = PyArg_ParseTupleAndKeywords(args, kwds, "|O!iO!", keywords,
					  &PaneType, &doc,
					  &view,
					  &MarkType, &orig);
	if (ret <= 0)
		return -1;
	if (doc && orig) {
		PyErr_SetString(PyExc_TypeError, "Only one of 'pane' and 'orig' may be set");
		return -1;
	}
	if (!doc && !orig) {
		PyErr_SetString(PyExc_TypeError, "At least one of 'pane' and 'orig' must be set");
		return -1;
	}
	if (doc) {
		struct pane *p = doc->pane;
		self->mark = vmark_new(p, view);
		local = p->handle &&
			p->handle->func == python_doc_call.func;
	} else {
		self->mark = mark_dup(orig->mark, 0);
		local = orig->local;
	}
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark creation failed");
		return -1;
	}
	if (self->mark->viewnum >= 0) {
		/* vmarks don't disappear until explicitly released */
		Py_INCREF(self);
		self->released = 0;
	} else
		self->released = 1;
	self->local = local;
	self->mark->mtype = &MarkType;
	self->mark->mdata = (PyObject*)self;
	return 1;
}

static void mark_dealloc(Mark *self)
{
	if (self->mark && self->mark->mtype == &MarkType) {
		struct mark *m = self->mark;
		self->mark = NULL;
		m->mtype = NULL;
		m->mdata = NULL;
		if (self->local && m->ref.c)
			Py_DECREF(m->ref.c);
		mark_free(m);
	}
	self->ob_type->tp_free((PyObject*)self);
}

static PyObject *Mark_to_mark(Mark *self, PyObject *args)
{
	Mark *other = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &MarkType, &other);
	if (ret <= 0)
		return NULL;
	mark_to_mark(self->mark, other->mark);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_next(Mark *self)
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
		return Mark_Frommark(next, self->local);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_prev(Mark *self)
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
		return Mark_Frommark(prev, self->local);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_next_any(Mark *self)
{
	struct mark *next;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	next = doc_next_mark_all(self->mark);
	if (next)
		return Mark_Frommark(next, self->local);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_dup(Mark *self)
{
	struct mark *new;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	new = mark_dup(self->mark, 1);
	if (new) {
		if (self->mark->mtype != &MarkType)
			return Mark_Frommark(new, self->local);
		new->mdata = Mark_Frommark(new, self->local);
		new->mtype = &MarkType;
		Py_INCREF(new->mdata);
		return new->mdata;
	}
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_release(Mark *self)
{
	if (self->mark && self->mark->viewnum >= 0 &&
	    self->released == 0 &&
	    self->mark->mtype == &MarkType) {
		struct mark *m = self->mark;
		Py_DECREF(self);
		m->mdata = NULL;
		m->mtype = NULL;
		self->mark = NULL;
		mark_free(m);
		self->released = 1;
	}
	Py_INCREF(Py_None);
	return Py_None;
}

static PyMethodDef mark_methods[] = {
	{"to_mark", (PyCFunction)Mark_to_mark, METH_VARARGS,
	 "Move one mark to another"},
	{"next", (PyCFunction)Mark_next, METH_NOARGS,
	 "next vmark"},
	{"prev", (PyCFunction)Mark_prev, METH_NOARGS,
	 "previous vmark"},
	{"next_any", (PyCFunction)Mark_next_any, METH_NOARGS,
	 "next any_mark"},
	{"dup", (PyCFunction)Mark_dup, METH_NOARGS,
	 "duplicate a mark, preserving type"},
	{"release", (PyCFunction)Mark_release, METH_NOARGS,
	 "release a vmark so it can disappear"},
	{NULL}
};

static PyObject *mark_get_item(Mark *self, PyObject *key)
{
	char *k, *v;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	if (!PyString_Check(key)) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string");
		return NULL;
	}
	k = PyString_AsString(key);
	v = attr_find(self->mark->attrs, k);
	if (v)
		return Py_BuildValue("s", v);
	Py_INCREF(Py_None);
	return Py_None;
}

static int mark_set_item(Mark *self, PyObject *key, PyObject *val)
{
	char *k, *v;
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	if (!PyString_Check(key)) {
		PyErr_SetString(PyExc_TypeError, "Key must be a string");
		return -1;
	}
	if (!PyString_Check(val)) {
		PyErr_SetString(PyExc_TypeError, "value must be a string");
		return -1;
	}
	k = PyString_AsString(key);
	v = PyString_AsString(val);
	attr_set_str(&self->mark->attrs, k, v);
	return 0;
}

static PyMappingMethods mark_mapping = {
	.mp_length = NULL,
	.mp_subscript = (binaryfunc)mark_get_item,
	.mp_ass_subscript = (objobjargproc)mark_set_item,
};

static PyTypeObject MarkType = {
    PyObject_HEAD_INIT(NULL)
    0,				/*ob_size*/
    "edlib.Mark",		/*tp_name*/
    sizeof(Mark),		/*tp_basicsize*/
    0,				/*tp_itemsize*/
    (destructor)mark_dealloc,	/*tp_dealloc*/
    NULL,			/*tp_print*/
    NULL,			/*tp_getattr*/
    NULL,			/*tp_setattr*/
    NULL,			/*tp_compare*/
    NULL,			/*tp_repr*/
    NULL,			/*tp_as_number*/
    NULL,			/*tp_as_sequence*/
    &mark_mapping,		/*tp_as_mapping*/
    NULL,			/*tp_hash */
    NULL,			/*tp_call*/
    NULL,			/*tp_str*/
    NULL,			/*tp_getattro*/
    NULL,			/*tp_setattro*/
    NULL,			/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "edlib marks",		/* tp_doc */
    NULL,			/* tp_traverse */
    NULL,			/* tp_clear */
    (richcmpfunc)mark_compare,	/* tp_richcompare */
    0,				/* tp_weaklistoffset */
    NULL,			/* tp_iter */
    NULL,			/* tp_iternext */
    mark_methods,		/* tp_methods */
    NULL,			/* tp_members */
    mark_getseters,		/* tp_getset */
    NULL,			/* tp_base */
    NULL,			/* tp_dict */
    NULL,			/* tp_descr_get */
    NULL,			/* tp_descr_set */
    0,				/* tp_dictoffset */
    .tp_init = (initproc)Mark_init,/* tp_init */
    NULL,			/* tp_alloc */
    .tp_new = (newfunc)mark_new,/* tp_new */
};

static void comm_dealloc(Comm *self)
{
	self->ob_type->tp_free((PyObject*)self);
}

static PyObject *comm_repr(Comm *p)
{
	char buf[50];
	sprintf(buf, "<comm-0x%p/0x%p>", p->comm, p->comm->func);
	return Py_BuildValue("s", buf);
}

static PyObject *Comm_call(Comm *c, PyObject *args, PyObject *kwds)
{
	struct cmd_info ci = {};
	int rv;

#if 0
	if (c->comm->func == python_call.func) {
		struct python_command *pc = container_of(c->comm, struct python_command,
							 c);
		return PyObject_Call(pc->callable, args, kwds);
	}
#endif
	rv = get_cmd_info(&ci, args, kwds);
	if (rv <= 0)
		return NULL;
	ci.comm = c->comm;
	rv = c->comm->func(&ci);
	if (!rv) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (rv < 0) {
		PyErr_SetObject(Edlib_CommandFailed, PyInt_FromLong(rv));
		return NULL;
	}
	return PyInt_FromLong(rv);
}

static Comm *comm_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	Comm *self;

	self = (Comm *)type->tp_alloc(type, 0);
	if (self)
		self->comm = NULL;
	return self;
}

static PyTypeObject CommType = {
    PyObject_HEAD_INIT(NULL)
    0,				/*ob_size*/
    "edlib.Comm",		/*tp_name*/
    sizeof(Comm),		/*tp_basicsize*/
    0,				/*tp_itemsize*/
    (destructor)comm_dealloc,	/*tp_dealloc*/
    NULL,			/*tp_print*/
    NULL,			/*tp_getattr*/
    NULL,			/*tp_setattr*/
    NULL,			/*tp_compare*/
    (reprfunc)comm_repr,	/*tp_repr*/
    NULL,			/*tp_as_number*/
    NULL,			/*tp_as_sequence*/
    NULL,			/*tp_as_mapping*/
    NULL,			/*tp_hash */
    (ternaryfunc)Comm_call,	/*tp_call*/
    NULL,			/*tp_str*/
    NULL,			/*tp_getattro*/
    NULL,			/*tp_setattro*/
    NULL,			/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "edlib command",		/* tp_doc */
    NULL,			/* tp_traverse */
    NULL,			/* tp_clear */
    NULL,			/* tp_richcompare */
    0,				/* tp_weaklistoffset */
    NULL,			/* tp_iter */
    NULL,			/* tp_iternext */
    NULL,			/* tp_methods */
    NULL,			/* tp_members */
    NULL,			/* tp_getset */
    NULL,			/* tp_base */
    NULL,			/* tp_dict */
    NULL,			/* tp_descr_get */
    NULL,			/* tp_descr_set */
    0,				/* tp_dictoffset */
    NULL,			/* tp_init */
    NULL,			/* tp_alloc */
    .tp_new = (newfunc)comm_new,/* tp_new */
};

/* The 'call' function takes liberties with arg passing.
 * Positional args must start with the key, and then are handled based on
 * there type.  They can be panes, strigns, ints, pairs, marks, commands.
 * Panes are assigned to focus, then home.
 * Strings (after the key) are assigned to 'str' then 'str2'.
 * Ints are assigned to numeric then extra
 * Pairs (must be of ints) are assigned to x,y then hx,hy.
 * Marks are assigned to mark then mark2
 * A command is assigned to comm2 (comm is set automatically)
 * kwd arguments can also be used, currently
 * key, home, focus, xy, hxy, str, str2, mark, mark2, comm2.
 */
static int get_cmd_info(struct cmd_info *ci, PyObject *args, PyObject *kwds)
{
	int argc;
	PyObject *a;
	int i;
	int numeric_set = 0, extra_set = 0;
	int xy_set = 0;

	if (!PyTuple_Check(args))
		return 0;
	argc = PyTuple_GET_SIZE(args);
	if (argc >= 1) {
		/* First positional arg must be the key */
		a = PyTuple_GetItem(args, 0);
		if (!PyString_Check(a)) {
			PyErr_SetString(PyExc_TypeError, "First are must be key");
			return 0;
		}
		ci->key = PyString_AsString(a);
	}
	for (i = 1; i < argc; i++) {
		a = PyTuple_GetItem(args, i);
		if (PyObject_TypeCheck(a, &PaneType)) {
			if (ci->home == NULL)
				ci->home = ((Pane*)a)->pane;
			else if (ci->focus == NULL)
				ci->focus = ((Pane*)a)->pane;
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
		} else if (PyString_Check(a)) {
			if (ci->str == NULL)
				ci->str = PyString_AsString(a);
			else if (ci->str2 == NULL)
				ci->str2 = PyString_AsString(a);
			else {
				PyErr_SetString(PyExc_TypeError, "Only 3 String args permitted");
				return 0;
			}
		} else if (PyInt_Check(a)) {
			if (!numeric_set) {
				ci->numeric = PyInt_AsLong(a);
				numeric_set = 1;
			} else if (!extra_set) {
				ci->extra = PyInt_AsLong(a);
				extra_set = 1;
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
			if (!PyInt_Check(n1) || !PyInt_Check(n2)) {
				PyErr_SetString(PyExc_TypeError, "Only tuples of integers permitted");
				return 0;
			}
			if (!xy_set) {
				ci->x = PyInt_AsLong(n1);
				ci->y = PyInt_AsLong(n2);
				xy_set = 1;
			} else {
				PyErr_SetString(PyExc_TypeError, "Only one tuple permitted");
				return 0;
			}
		} else if (PyObject_TypeCheck(a, &CommType)) {
			Comm *c = (Comm*)a;
			if (ci->comm2 == NULL)
				ci->comm2 = c->comm;
			else {
				PyErr_SetString(PyExc_TypeError, "Only one callable permitted");
				return 0;
			}
		} else if (PyCallable_Check(a)) {
			/* FIXME this is never freed */
			struct python_command *pc = malloc(sizeof(*pc));
			Py_INCREF(a);
			pc->callable = a;
			pc->c = python_call;
			if (ci->comm2 == NULL)
				ci->comm2 = &pc->c;
			else {
				free(pc);
				PyErr_SetString(PyExc_TypeError, "Only one callable permitted");
				return 0;
			}
		} else {
			PyErr_SetString(PyExc_TypeError, "Unsupported arg type");
			return 0;
		}
	}
	/* Handle keyword args - later */
	if (!ci->key) {
		PyErr_SetString(PyExc_TypeError, "No key specified");
		return 0;
	}
	if (!ci->home) {
		PyErr_SetString(PyExc_TypeError, "No pane specified");
		return 0;
	}
	if (!ci->focus)
		ci->focus = ci->home;

	return xy_set ? 2 : 1;
}

void edlib_init(struct pane *ed)
{
	PyObject *m;

	Py_SetProgramName("edlib");
	Py_Initialize();

	PaneType.tp_new = PyType_GenericNew;
	DocType.tp_new = PyType_GenericNew;
	MarkType.tp_new = PyType_GenericNew;
	CommType.tp_new = PyType_GenericNew;
	if (PyType_Ready(&PaneType) < 0)
		return;
	if (PyType_Ready(&DocType) < 0)
		return;
	if (PyType_Ready(&MarkType) < 0)
		return;
	if (PyType_Ready(&CommType) < 0)
		return;

	m = Py_InitModule3("edlib", NULL,
			   "edlib - one more editor is never enough.");

	if (!m)
		return;

	Py_INCREF(&PaneType);
	Py_INCREF(&MarkType);
	PyModule_AddObject(m, "Pane", (PyObject *)&PaneType);
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
	PyModule_AddIntConstant(m, "WEOF", 0x1FFFFFF);
	call_comm("global-set-command", ed, 0, NULL, "python-load", 0, &python_load);
	call_comm("global-set-command", ed, 0, NULL, "global-load-modules:python", 0,
		  &python_load_module);

	Edlib_CommandFailed = PyErr_NewException("edlib.commandfailed", NULL, NULL);
	Py_INCREF(Edlib_CommandFailed);
	PyModule_AddObject(m, "commandfailed", Edlib_CommandFailed);
	EdlibModule = m;
}
