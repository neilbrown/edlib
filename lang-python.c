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
 *  edlib.mark  - these reference locations in a document.  The document is
 *                not directly accessible, it can only be accessed through
 *                a pane (which may translate events and results).
 *                get/set operations for rpos.
 *                get/set for viewnum (cannot be set)
 *                iterator for both 'all' and 'view' lists.
 *                no methods yet.
 *
 *
 */

#include <Python.h>
#include <structmember.h>

#include "core.h"

PyObject *Edlib_CommandFailed;
PyObject *EdlibModule;

typedef struct {
	PyObject_HEAD
	struct pane	*pane;
} Pane;
static PyTypeObject PaneType;

typedef struct {
	PyObject_HEAD
	struct mark	*mark;
	short		iter_type;
} Mark;
static PyTypeObject MarkType;
#define	ITER_ALL	1
#define	ITER_VIEW	2
#define	ITER_VIEW_REVERSE 3

static inline PyObject *Pane_Frompane(struct pane *p)
{
	Pane *pane = (Pane *)PyObject_CallObject((PyObject*)&PaneType, NULL);
	pane->pane = p;
	return (PyObject*)pane;
}

static inline PyObject *Mark_Frommark(struct mark *m)
{
	Mark *mark = (Mark *)PyObject_CallObject((PyObject*)&MarkType, NULL);
	mark->mark = m;
	return (PyObject*)mark;
}

DEF_CMD(python_load)
{
	char *fname = ci->str;
	FILE *fp;
	PyObject *globals;
	struct editor *ed = pane2ed(ci->home);
	PyObject *Ed;

	if (!fname)
		return -1;
	fp = fopen(fname, "r");
	if (!fp)
		return -1;

	Ed = Pane_Frompane(&ed->root);
	globals = PyDict_New();
	PyDict_SetItemString(globals, "editor", Ed);
	PyDict_SetItemString(globals, "pane", Pane_Frompane(ci->home));
	PyDict_SetItemString(globals, "edlib", EdlibModule);
	PyRun_FileExFlags(fp, fname, Py_file_input, globals, globals, 0, NULL);
	PyErr_Print();
	Py_DECREF(Ed);
	Py_DECREF(globals);
	fclose(fp);
	return 1;
}

/* When a python callable is passed to a edlib_call() we combine it
 * with this "python_call" to edlib to call back that callable.
 */
struct python_command {
	struct command	c;
	PyObject	*callable;
};

DEF_CMD(python_call)
{
	struct python_command *pc = container_of(ci->comm, struct python_command, c);
	PyObject *ret, *args, *kwds;
	int rv = 1;

	args = Py_BuildValue("(s)", ci->str);
	kwds = PyDict_New();
	if (ci->focus)
		PyDict_SetItemString(kwds, "focus", Pane_Frompane(ci->focus));
	if (ci->home)
		PyDict_SetItemString(kwds, "home", Pane_Frompane(ci->home));
	if (ci->mark)
		PyDict_SetItemString(kwds, "mark", Mark_Frommark(ci->mark));
	if (ci->mark2)
		PyDict_SetItemString(kwds, "mark2", Mark_Frommark(ci->mark2));
	if (ci->str)
		PyDict_SetItemString(kwds, "str", Py_BuildValue("s", ci->str));
	if (ci->str2)
		PyDict_SetItemString(kwds, "str2", Py_BuildValue("s", ci->str2));
	PyDict_SetItemString(kwds, "numeric", Py_BuildValue("i", ci->numeric));
	PyDict_SetItemString(kwds, "extra", Py_BuildValue("i", ci->extra));
	PyDict_SetItemString(kwds, "xy", Py_BuildValue("ii", ci->x, ci->y));
	PyDict_SetItemString(kwds, "hxy", Py_BuildValue("ii", ci->hx, ci->hy));
	/* FIXME what about ->comm and ->comm2?? */

	ret = PyObject_Call(pc->callable, args, kwds);

	Py_DECREF(args);
	Py_DECREF(kwds);
	if (!ret) {
		/* FIXME cancel error?? */
		return -1;
	}
	if (PyDict_Check(ret)) {
		PyObject *r;
		r = PyDict_GetItemString(ret, "focus");
		if (r && Py_TYPE(r) == &PaneType)
			ci->focus = ((Pane*)r)->pane;
		r = PyDict_GetItemString(ret, "home");
		if (r && Py_TYPE(r) == &PaneType)
			ci->home = ((Pane*)r)->pane;

		r = PyDict_GetItemString(ret, "mark");
		if (r && Py_TYPE(r) == &MarkType)
			ci->mark = ((Mark*)r)->mark;
		r = PyDict_GetItemString(ret, "mark2");
		if (r && Py_TYPE(r) == &MarkType)
			ci->mark2 = ((Mark*)r)->mark;

		r = PyDict_GetItemString(ret, "str");
		if (r && PyString_Check(r))
			ci->str = PyString_AsString(r);
		r = PyDict_GetItemString(ret, "str2");
		if (r && PyString_Check(r))
			ci->str2 = PyString_AsString(r);

		r = PyDict_GetItemString(ret, "numeric");
		if (r && PyInt_Check(r))
			ci->numeric = PyInt_AsLong(r);
		r = PyDict_GetItemString(ret, "extra");
		if (r && PyInt_Check(r))
			ci->extra = PyInt_AsLong(r);

		/* FIXME xy and hxy */
	}
	Py_DECREF(ret);
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

static PyMethodDef pane_methods[] = {
	{"children", (PyCFunction)pane_children, METH_NOARGS,
	 "provides and iterator which will iterate over all children"},
	{"take_focus", (PyCFunction)Pane_focus, METH_NOARGS,
	 "Claim the focus for this pane"},
	{"refresh", (PyCFunction)Pane_refresh, METH_NOARGS,
	 "Trigger refresh on this pane"},
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
	Pane *newpane = (Pane *)Pane_Frompane(NULL);

	if (p->pane == NULL) {
		Py_DECREF(newpane);
		PyErr_SetString(PyExc_TypeError, "pane not initialized");
		return NULL;
	}
	if (*which == 'p')
		newpane->pane = p->pane->parent;
	if (*which == 'f')
		newpane->pane = p->pane->focus;
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
    {"parent",
     (getter)pane_getpane, (setter)pane_nosetpane,
     "Parent pane", "p"},
    {"focus",
     (getter)pane_getpane, (setter)pane_nosetpane,
     "Focal child", "f"},
    {NULL}  /* Sentinel */
};

static PyTypeObject PaneType = {
    PyObject_HEAD_INIT(NULL)
    0,				/*ob_size*/
    "edlib.Pane",		/*tp_name*/
    sizeof(Pane),		/*tp_basicsize*/
    0,				/*tp_itemsize*/
    (destructor)pane_dealloc,	/*tp_dealloc*/
    0,				/*tp_print*/
    0,				/*tp_getattr*/
    0,				/*tp_setattr*/
    0,				/*tp_compare*/
    (reprfunc)pane_repr,	/*tp_repr*/
    0,				/*tp_as_number*/
    0,				/*tp_as_sequence*/
    0,				/*tp_as_mapping*/
    0,				/*tp_hash */
    0,				/*tp_call*/
    0,				/*tp_str*/
    0,				/*tp_getattro*/
    0,				/*tp_setattro*/
    0,				/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "edlib panes",		/* tp_doc */
    0,				/* tp_traverse */
    0,				/* tp_clear */
    0,				/* tp_richcompare */
    0,				/* tp_weaklistoffset */
    (getiterfunc)pane_this,	/* tp_iter */
    (iternextfunc)pane_next,	/* tp_iternext */
    pane_methods,		/* tp_methods */
    0,				/* tp_members */
    pane_getseters,		/* tp_getset */
    0,				/* tp_base */
    0,				/* tp_dict */
    0,				/* tp_descr_get */
    0,				/* tp_descr_set */
    0,				/* tp_dictoffset */
    0,				/* tp_init */
    0,				/* tp_alloc */
    .tp_new = (newfunc)pane_new,/* tp_new */
};

static PyObject *mark_getrpos(Mark *m, void *x)
{
	if (m->mark == NULL) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	return PyInt_FromLong(m->mark->rpos);
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
	m->mark->rpos = val;
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

PyObject *mark_compare(Mark *a, Mark *b, int op)
{
	int ret = 0;
	switch(op) {
	case Py_LT: ret = mark_ordered(a->mark, b->mark); break;
	case Py_LE: ret = mark_ordered(a->mark, b->mark); break;
	case Py_GT: ret = mark_ordered(a->mark, b->mark); break;
	case Py_GE: ret = mark_ordered(a->mark, b->mark); break;
	case Py_EQ: ret = mark_ordered(a->mark, b->mark); break;
	case Py_NE: ret = mark_ordered(a->mark, b->mark); break;
	}
	return ret ? Py_True : Py_False;
}

static Mark *mark_this(Mark *self)
{
	Mark *ret = (Mark*)Mark_Frommark(self->mark);
	ret->iter_type = ITER_ALL;
	return ret;
}

static Mark *Mark_next(Mark *self)
{
	struct mark *next;

	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	switch (self->iter_type) {
	default:
	case ITER_ALL: next = doc_next_mark_all(self->mark); break;
	case ITER_VIEW: next = vmark_next(self->mark); break;
	case ITER_VIEW_REVERSE: next = vmark_prev(self->mark); break;
	}
	self->mark = next;
	if (next == NULL) {
		/* Reached the end of the list */
		return NULL;
	}

	return (Mark*)Mark_Frommark(next);
}

static PyGetSetDef mark_getseters[] = {
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

static void mark_dealloc(Mark *self)
{
	self->ob_type->tp_free((PyObject*)self);
}

static PyTypeObject MarkType = {
    PyObject_HEAD_INIT(NULL)
    0,				/*ob_size*/
    "edlib.Mark",		/*tp_name*/
    sizeof(Mark),		/*tp_basicsize*/
    0,				/*tp_itemsize*/
    (destructor)mark_dealloc,	/*tp_dealloc*/
    0,				/*tp_print*/
    0,				/*tp_getattr*/
    0,				/*tp_setattr*/
    0,				/*tp_compare*/
    0,				/*tp_repr*/
    0,				/*tp_as_number*/
    0,				/*tp_as_sequence*/
    0,				/*tp_as_mapping*/
    0,				/*tp_hash */
    0,				/*tp_call*/
    0,				/*tp_str*/
    0,				/*tp_getattro*/
    0,				/*tp_setattro*/
    0,				/*tp_as_buffer*/
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE, /*tp_flags*/
    "edlib marks",		/* tp_doc */
    0,				/* tp_traverse */
    0,				/* tp_clear */
    (richcmpfunc)mark_compare,	/* tp_richcompare */
    0,				/* tp_weaklistoffset */
    (getiterfunc)mark_this,	/* tp_iter */
    (iternextfunc)Mark_next,	/* tp_iternext */
    0,				/* tp_methods */
    0,				/* tp_members */
    mark_getseters,		/* tp_getset */
    0,				/* tp_base */
    0,				/* tp_dict */
    0,				/* tp_descr_get */
    0,				/* tp_descr_set */
    0,				/* tp_dictoffset */
    0,				/* tp_init */
    0,				/* tp_alloc */
    .tp_new = (newfunc)mark_new,/* tp_new */
};

/* The 'call' function takes liberties with arg passing.
 * Positional args must start with the key, and then are handled based on
 * there type.  They can be panes, strigns, ints, pairs, marks, commands.
 * Panes are assigned to focus, then home.
 * Strings (after the key) are assigned to 'str' then 'str2'.
 * Ints are assigned to numeric then extra
 * Pairs (must be of ints) are assigned to x,y then hx,hy.
 * Marks are assigned to mark then mark2
 * commands are assigned to comm2, then comm (yes - backwards).
 * kwd arguments can also be used, currently
 * key, home, focus, xy, hxy, str, str2, mark, mark2, comm, comm2.
 */
static PyObject *edlib_call(PyObject *self, PyObject *args, PyObject *kwds)
{
	struct cmd_info ci = {0};
	int argc;
	PyObject *a;
	int i;
	int rv;
	int numeric_set = 0, extra_set = 0;
	int xy_set = 0, hxy_set = 0;

	if (!PyTuple_Check(args))
		return NULL;
	argc = PyTuple_GET_SIZE(args);
	if (argc >= 1) {
		/* First positional arg must be the key */
		a = PyTuple_GetItem(args, 0);
		if (!PyString_Check(a)) {
			PyErr_SetString(PyExc_TypeError, "First are must be key");
			return NULL;
		}
		ci.key = PyString_AsString(a);
	}
	for (i = 1; i < argc; i++) {
		a = PyTuple_GetItem(args, i);
		if (Py_TYPE(a) == &PaneType) {
			if (ci.focus == NULL)
				ci.focus = ((Pane*)a)->pane;
			else if (ci.home == NULL)
				ci.home = ((Pane*)a)->pane;
			else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Pane args permitted");
				return NULL;
			}
		} else if (Py_TYPE(a) == &MarkType) {
			if (ci.mark == NULL)
				ci.mark = ((Mark*)a)->mark;
			else if (ci.mark2 == NULL)
				ci.mark2 = ((Mark*)a)->mark;
			else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Mark args permitted");
				return NULL;
			}
		} else if (PyString_Check(a)) {
			if (ci.str == NULL)
				ci.str = PyString_AsString(a);
			else if (ci.str2 == NULL)
				ci.str2 = PyString_AsString(a);
			else {
				PyErr_SetString(PyExc_TypeError, "Only 3 String args permitted");
				return NULL;
			}
		} else if (PyInt_Check(a)) {
			if (!numeric_set) {
				ci.numeric = PyInt_AsLong(a);
				numeric_set = 1;
			} else if (!extra_set) {
				ci.extra = PyInt_AsLong(a);
				extra_set = 1;
			} else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Number args permitted");
				return NULL;
			}
		} else if (PyTuple_Check(a)) {
			int n = PyTuple_GET_SIZE(a);
			PyObject *n1, *n2;
			if (n != 2) {
				PyErr_SetString(PyExc_TypeError, "Only 2-element tuples permitted");
				return NULL;
			}
			n1 = PyTuple_GetItem(a, 0);
			n2 = PyTuple_GetItem(a, 1);
			if (!PyInt_Check(n1) || !PyInt_Check(n2)) {
				PyErr_SetString(PyExc_TypeError, "Only tuples of integers permitted");
				return NULL;
			}
			if (!xy_set) {
				ci.x = PyInt_AsLong(n1);
				ci.y = PyInt_AsLong(n2);
				xy_set = 1;
			} else if (!hxy_set) {
				ci.hx = PyInt_AsLong(n1);
				ci.hy = PyInt_AsLong(n2);
				hxy_set = 1;
			} else {
				PyErr_SetString(PyExc_TypeError, "Only two tuples permitted");
				return NULL;
			}
		} else if (PyCallable_Check(a)) {
			/* FIXME this is never freed */
			struct python_command *pc = malloc(sizeof(*pc));
			pc->callable = a;
			pc->c = python_call;
			if (ci.comm2 == NULL)
				ci.comm2 = &pc->c;
			else if (ci.comm == NULL)
				ci.comm = &pc->c;
			else {
				free(pc);
				PyErr_SetString(PyExc_TypeError, "Only two callables permitted");
				return NULL;
			}
		} else {
			PyErr_SetString(PyExc_TypeError, "Unsupported arg type");
			return NULL;
		}
	}
	/* Handle keyword args - later */
	if (!ci.key) {
		PyErr_SetString(PyExc_TypeError, "No key specified");
		return NULL;
	}
	if (!ci.focus) {
		PyErr_SetString(PyExc_TypeError, "No focus specified");
		return NULL;
	}

	if (xy_set)
		rv = key_handle_xy(&ci);
	else
		rv = key_handle_focus(&ci);
	if (!rv) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	if (rv < 0) {
		PyErr_SetString(Edlib_CommandFailed, ci.str ? ci.str : "Command Failed");
		return NULL;
	}
	/* FIXME how do I return the various return values? */
	Py_INCREF(Py_True);
	return Py_True;
}

static PyMethodDef edlib_methods[] = {
	{"call", (PyCFunction)edlib_call, METH_VARARGS | METH_KEYWORDS,
	 "Make an edlib call"},
	{NULL, NULL, 0, NULL}
};

void edlib_init(struct editor *ed)
{
	PyObject *m;

	Py_SetProgramName("edlib");
	Py_Initialize();

	PaneType.tp_new = PyType_GenericNew;
	MarkType.tp_new = PyType_GenericNew;
	if (PyType_Ready(&PaneType) < 0)
		return;
	if (PyType_Ready(&MarkType) < 0)
		return;

	m = Py_InitModule3("edlib", edlib_methods,
			   "edlib - one more editor is never enough.");

	if (!m)
		return;

	Py_INCREF(&PaneType);
	Py_INCREF(&MarkType);
	PyModule_AddObject(m, "pane", (PyObject *)&PaneType);
	PyModule_AddObject(m, "mark", (PyObject *)&MarkType);
	key_add(ed->commands, "python-load", &python_load);

	Edlib_CommandFailed = PyErr_NewException("edlib.commandfailed", NULL, NULL);
	Py_INCREF(Edlib_CommandFailed);
	PyModule_AddObject(m, "commandfailed", Edlib_CommandFailed);
	EdlibModule = m;
}
