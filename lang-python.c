/*
 * Copyright Neil Brown ©2015-2023 <neil@brown.name>
 * May be distributed under terms of GPLv2 - see file:COPYING
 *
 * Python3 bindings for edlib.
 * An edlib command "global-load-modules:python" will read and execute
 * a python module.
 * It must "import edlib" and it can use "edlib.editor" to get the editor
 * instance.
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

//#define PyType_HasFeature __PyType_HasFeature
#include <Python.h>
#undef PyType_HasFeature __PyType_HasFeature
int PyType_HasFeature(PyTypeObject *type, unsigned long feature);

#undef Py_INCREF
#define Py_INCREF(op) (0)
#undef Py_DECREF
#define Py_DECREF(op) (0)
#undef Py_XDECREF
#define Py_XDECREF(op) (0)
#undef Py_IS_TYPE
#define Py_IS_TYPE(ob, type) (ob == (void*)type)
#else /* CHECKER */
#include <Python.h>
#endif
struct Mark;
#define MARK_DATA_PTR struct Mark
#define PRIVATE_DOC_REF

struct doc_ref {
	PyObject *c;
	int o;
};

#include <fcntl.h>
#include <signal.h>

#define DOC_DATA_TYPE struct python_doc
#define PANE_DATA_PTR_TYPE struct Pane *
struct Pane;
#include "core.h"
#include "misc.h"
#include "rexel.h"

struct python_doc {
	struct doc	doc;
	struct pydoc	*pdoc;
};

#include "core-pane.h"

#define SAFE_CI {.key=safe_cast NULL,\
		 .home=safe_cast NULL,\
		 .focus=safe_cast NULL,\
		 .comm=safe_cast NULL,\
	}

static PyObject *Edlib_CommandFailed;
static PyObject *EdlibModule;

static struct pane *ed_pane;

static char *module_dir;
static char *python_as_string(PyObject *s, PyObject **tofree safe);

/* Python commands visible to edlib are wrapped in
 * the python_command.  There is only one per callback,
 * so that edlib code can compare for equalty.
 */
struct python_command {
	struct command	c;
	PyObject	*callable;
	struct list_head lst;
};
static LIST_HEAD(exported_commands);

DEF_CMD(python_call);
DEF_CMD(python_pane_call);
DEF_CMD(python_doc_call);
static void python_free_command(struct command *c safe);

static struct python_command *export_callable(PyObject *callable safe,
					      PyObject *nm)
{
	struct python_command *c;
	const char *name = NULL;
	int len;

	if (nm && PyUnicode_Check(nm))
		name = PyUnicode_AsUTF8(nm);
	if (!name)
		name = "";
	len = strlen(name);

	list_for_each_entry(c, &exported_commands, lst)
		if (c->callable == callable) {
			command_get(&c->c);
			return c;
		}

	c = malloc(sizeof(*c));
	c->c = python_call;
	c->c.free = python_free_command;
	c->c.refcnt = 0;
	if (strcmp(name, "handle_close") == 0 ||
	    (len > 10 &&
	     strcmp(name+len-10, "_closed_ok") == 0))
		c->c.closed_ok = 1;
	else
		c->c.closed_ok = 0;
	command_get(&c->c);
	Py_INCREF(callable);
	c->callable = callable;
	list_add(&c->lst, &exported_commands);
	return c;
}

typedef struct Pane {
	PyObject_HEAD;
	struct pane	*pane;
	struct command	cmd;
	struct map	*map;
	int		map_init;
} Pane;
static PyTypeObject PaneType;

typedef struct {
	PyObject_HEAD;
	struct pane	*pane;
} PaneIter;
static PyTypeObject PaneIterType;

typedef struct pydoc {
	PyObject_HEAD;
	struct pane	*pane;
	struct command	cmd;
	struct map	*map;
	int		map_init;
} Doc;
static PyTypeObject DocType;

typedef struct Mark {
	PyObject_HEAD;
	struct mark	*mark;
} Mark;
static PyTypeObject MarkType;
static void mark_refcnt(struct mark *m safe, int inc);

typedef struct {
	PyObject_HEAD;
	struct command	*comm;
} Comm;
static PyTypeObject CommType;

static inline bool pane_valid(Pane *p safe)
{
	if (p->pane && p->pane->handle)
		return True;
	PyErr_SetString(PyExc_TypeError, "Pane has been freed");
	return False;
}

static inline bool doc_valid(Doc *p safe)
{
	if (p->pane && p->pane->handle)
		return True;
	PyErr_SetString(PyExc_TypeError, "Doc pane has been freed");
	return False;
}

static bool get_cmd_info(struct cmd_info *ci safe, PyObject *args safe, PyObject *kwds,
			 PyObject **s1 safe, PyObject **s2 safe);

static int in_pane_frompane = 0;
static inline PyObject *safe Pane_Frompane(struct pane *p)
{
	Pane *pane;
	if (p && p->handle && p->handle->func == python_pane_call.func) {
		pane = p->data;
		Py_INCREF(pane);
	} else if (p && p->handle && p->handle->func == python_doc_call.func) {
		struct python_doc *pd = p->doc_data;
		Doc *pdoc = pd->pdoc;
		pane = (Pane*)pdoc;
		Py_INCREF(pane);
	} else {
		in_pane_frompane = 1;
		pane = (Pane *)PyObject_CallObject((PyObject*)&PaneType, NULL);
		in_pane_frompane = 0;
		if (!pane)
			;
		else if (p)
			pane->pane = pane_get(p);
		else
			pane->pane = p;
	}
	return (PyObject*)pane;
}

static inline PyObject *safe Mark_Frommark(struct mark *m safe)
{
	Mark *mark;

	if (mark_valid(m) && m->mtype == &MarkType && m->mdata) {
		/* This is a vmark, re-use the PyObject */
		Py_INCREF(m->mdata);
		return (PyObject*)m->mdata;
	}
	mark = (Mark *)PyObject_CallObject((PyObject*)&MarkType, NULL);
	if (mark && mark_valid(m))
		mark->mark = m;
	return (PyObject*)mark;
}

static inline PyObject *safe Comm_Fromcomm(struct command *c safe)
{
	Comm *comm = (Comm *)PyObject_CallObject((PyObject*)&CommType, NULL);
	if (comm)
		comm->comm = command_get(c);
	return (PyObject*)comm;
}

static PyObject *py_LOG(PyObject *self, PyObject *args);
static void PyErr_LOG(void)
{
	/* cribbed from https://groups.google.com/forum/#!topic/comp.lang.python/khLrxC6EOKc */
	char *errorMsg = NULL;
	PyObject *modIO = NULL;
	PyObject *modTB = NULL;
	PyObject *obFuncStringIO = NULL;
	PyObject *obIO = NULL;
	PyObject *obFuncTB = NULL;
	PyObject *argsTB = NULL;
	PyObject *obResult = NULL;
	PyObject *tofree = NULL;
	PyObject *exc_typ, *exc_val, *exc_tb;

	if (!PyErr_Occurred())
		return;

	PyErr_Fetch(&exc_typ, &exc_val, &exc_tb);
	PyErr_NormalizeException(&exc_typ, &exc_val, &exc_tb);
	if (exc_tb)
		PyException_SetTraceback(exc_val, exc_tb);

	/* Import the modules we need - StringIO and traceback */
	errorMsg = "Can't import io";
	modIO = PyImport_ImportModule("io");
	if (!modIO)
		goto out;

	errorMsg = "Can't import traceback";
	modTB = PyImport_ImportModule("traceback");
	if (!modTB)
		goto out;

	/* Construct a cStringIO object */
	errorMsg = "Can't find io.StringIO";
	obFuncStringIO = PyObject_GetAttrString(modIO, "StringIO");
	if (!obFuncStringIO)
		goto out;
	errorMsg = "io.StringIO() failed";
	obIO = PyObject_CallObject(obFuncStringIO, NULL);
	if (!obIO)
		goto out;

	/* Get the traceback.print_exception function, and call it. */
	errorMsg = "Can't find traceback.print_exception";
	obFuncTB = PyObject_GetAttrString(modTB, "print_exception");
	if (!obFuncTB)
		goto out;
	errorMsg = "can't make print_exception arguments";
	argsTB = Py_BuildValue("OOOOO",
			       exc_typ ? exc_typ : Py_None,
			       exc_val ? exc_val : Py_None,
			       exc_tb  ? exc_tb  : Py_None,
			       Py_None,
			       obIO);
	if (!argsTB)
		goto out;

	errorMsg = "traceback.print_exception() failed";
	obResult = PyObject_CallObject(obFuncTB, argsTB);
	if (!obResult) {
		PyErr_Print();
		goto out;
	}

	/* Now call the getvalue() method in the StringIO instance */
	Py_DECREF(obFuncStringIO);
	errorMsg = "cant find getvalue function";
	obFuncStringIO = PyObject_GetAttrString(obIO, "getvalue");
	if (!obFuncStringIO)
		goto out;
	Py_XDECREF(obResult);
	errorMsg = "getvalue() failed.";
	obResult = PyObject_CallObject(obFuncStringIO, NULL);
	if (!obResult)
		goto out;

	/* And it should be a string all ready to go - report it. */
	errorMsg = python_as_string(obResult, &tofree);;
	LOG("Python error:\n%s", errorMsg);
	if (errorMsg &&
	    (!ed_pane ||
	     call("editor:notify:Message:broadcast",ed_pane, 0, NULL,
		  "Python Error - see log") <= 0))
		/* Failed to alert anyone - write to stderr */
		fwrite(errorMsg, 1, strlen(errorMsg), stderr);
	Py_XDECREF(tofree);
	errorMsg = NULL;
out:
	if (errorMsg)
		LOG(errorMsg);
	Py_XDECREF(modIO);
	Py_XDECREF(modTB);
	Py_XDECREF(obFuncStringIO);
	Py_XDECREF(obIO);
	Py_XDECREF(obFuncTB);
	Py_XDECREF(argsTB);
	Py_XDECREF(obResult);

	Py_XDECREF(exc_typ);
	Py_XDECREF(exc_val);
	Py_XDECREF(exc_tb);
}

DEF_CMD(python_load_module)
{
	const char *name = ci->str;
	int fd;
	long long size;
	char *code;
	char buf[PATH_MAX];
	char buf2[PATH_MAX];
	PyObject *builtins, *compile, *args, *bytecode;

	if (!name)
		return Enoarg;
	snprintf(buf, sizeof(buf), "%s/python/%s.py", module_dir, name);
	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return Efail;
	size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	code = malloc(size+1);
	if (!code) {
		close(fd);
		return Efail;
	}
	size = read(fd, code, size);
	close(fd);
	if (size <= 0) {
		free(code);
		return Efail;
	}
	code[size] = 0;

	LOG("Loading python module %s from %s", name, buf);

	builtins = PyEval_GetBuiltins();
	compile = PyDict_GetItemString(builtins, "compile");
	args = safe_cast Py_BuildValue("(sss)", code, buf, "exec");
	bytecode = PyObject_Call(compile, args, NULL);
	Py_DECREF(args);
	free(code);
	if (bytecode == NULL) {
		PyErr_LOG();
		return Efail;
	}

	snprintf(buf2, sizeof(buf2), "edlib.%s", name);

	if (PyImport_ExecCodeModule(buf2, bytecode) == NULL)
		PyErr_LOG();
	Py_DECREF(bytecode);
	return 1;
}

static PyObject *safe python_string(const char *s safe, int len)
{
	const char *c = s;
	const char *e = NULL;
	wint_t wch;

	if (len >= 0)
		e = s + len;
	while ((!e || c < e) && *c && !(*c & 0x80))
		c++;
	while ((wch = get_utf8(&c, e)) != WEOF)
		if (wch == WERR || wch > 0x10FFFF)
			break;

	return safe_cast PyUnicode_DecodeUTF8(s, c - s, NULL);
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

static bool python_running;
DEF_CB(handle_alarm)
{
	if (python_running)
		PyErr_SetInterrupt();
	return 1;
}

REDEF_CB(python_call)
{
	struct python_command *pc = container_of(ci->comm, struct python_command, c);
	PyObject *ret = NULL, *args, *kwds, *str;
	int rv = 1;
	bool unterminated = False;
	int klen = strlen(ci->key);

	if (klen > 13 &&
	    strcmp(ci->key + klen - 13, " unterminated") == 0)
		unterminated = True;

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

	if (ci->str)
		str = python_string(ci->str, unterminated ? ci->num2 : -1);
	else {
		str = Py_None;
		Py_INCREF(Py_None);
	}
	if (str) {
		dict_add(kwds, "str", str);
		dict_add(kwds, "str1", str);
		Py_INCREF(str);
	} else {
		rv = 0;
	}

	rv = rv && dict_add(kwds, "str2",
			    ci->str2 ? python_string(ci->str2, -1):
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

	if (rv && pc->callable) {
		python_running = True;
		ret = PyObject_Call(pc->callable, args, kwds);
		python_running = False;
	}

	Py_DECREF(args);
	Py_DECREF(kwds);
	if (!ret) {
		PyErr_LOG();
		/* FIXME cancel error?? */
		return Efail;
	}
	if (ret == Py_None)
		rv = Efallthrough;
	else if (PyLong_Check(ret))
		rv = PyLong_AsLong(ret);
	else if (PyBool_Check(ret))
		rv = (ret == Py_True) ? 1 : Efalse;
	else if (PyUnicode_Check(ret) && PyUnicode_GET_LENGTH(ret) >= 1)
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
	PyObject *l = PyObject_Dir((PyObject*)self);
	int n;

	if (!self->map || !self->pane || !l)
		return;
	n = PyList_Size(l);
	/* First add the ranges, so individuals can over-ride them */
	for (i = 0; i < n ; i++) {
		PyObject *e = PyList_GetItem(l, i);
		PyObject *m = PyObject_GetAttr((PyObject*)self, e);

		if (m && PyMethod_Check(m)) {
			PyObject *doc = PyObject_GetAttrString(m, "__doc__");
			if (doc && doc != Py_None) {
				PyObject *tofree = NULL;
				char *docs = python_as_string(doc, &tofree);

				if (docs &&
				    strstarts(docs, "handle-range") &&
				    docs[12]) {
					char sep = docs[12];
					char *s1 = strchr(docs+13, sep);
					char *s2 = s1 ? strchr(s1+1, sep) : NULL;
					if (s2) {
						char *a = strndup(docs+13, s1-(docs+13));
						char *b = strndup(s1+1, s2-(s1+1));

						struct python_command *comm =
							export_callable(m, e);
						key_add_range(self->map, a, b,
							      &comm->c);
						free(a); free(b);
						command_put(&comm->c);
					}
				}
				if (docs &&
				    strstarts(docs, "handle-prefix:")) {
					char *a = strconcat(self->pane, docs+14);
					char *b = strconcat(self->pane,
							    a, "\xFF\xFF\xFF\xFF");
					struct python_command *comm =
						export_callable(m, e);
					key_add_range(self->map, a, b,
						      &comm->c);
					command_put(&comm->c);
				}
				Py_XDECREF(tofree);
			}
			Py_XDECREF(doc);
		}
		Py_XDECREF(m);
	}
	/* Now add the non-ranges */
	for (i = 0; i < n ; i++) {
		PyObject *e = PyList_GetItem(l, i);
		PyObject *m = PyObject_GetAttr((PyObject*)self, e);

		if (m && PyMethod_Check(m)) {
			PyObject *doc = PyObject_GetAttrString(m, "__doc__");
			char *docs;
			PyObject *tofree = NULL;
			if (doc && doc != Py_None &&
			    (docs = python_as_string(doc, &tofree)) != NULL) {

				if (strstarts(docs, "handle:")) {
					struct python_command *comm =
						export_callable(m, e);
					key_add(self->map, docs+7, &comm->c);
					command_put(&comm->c);
				}
				if (strstarts(docs, "handle-list") &&
				    docs[11]) {
					char sep = docs[11];
					char *s1 = docs + 12;
					while (s1 && *s1 && *s1 != sep) {
						struct python_command *comm =
							export_callable(m, e);
						char *a;
						char *s2 = strchr(s1, sep);
						if (s2) {
							a = strndup(s1, s2-s1);
							s1 = s2+1;
						} else {
							a = strdup(s1);
							s1 = NULL;
						}
						key_add(self->map, a, &comm->c);
						free(a);
						command_put(&comm->c);
					}
				}
			}
			Py_XDECREF(tofree);
			Py_XDECREF(doc);
		}
		Py_XDECREF(m);
	}
	Py_XDECREF(l);
	self->map_init = 1;
}

REDEF_CB(python_pane_call)
{
	Pane *home = container_of(ci->comm, Pane, cmd);

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
	Pane *p = container_of(c, Pane, cmd);
	struct pane *pn = p->pane;
	/* pane has been closed */
	p->pane = NULL;
	if (p->map)
		key_free(p->map);
	p->map = NULL;
	if (pn && PyObject_TypeCheck(p, &DocType))
		doc_free(&pn->doc_data->doc, safe_cast pn);
	if (pn)
		pane_put(pn);
	Py_DECREF(p);
}

DEF_CMD_CLOSED(python_close_mark)
{
	struct mark *m = ci->mark;

	if (m && m->viewnum >= 0 && m->mtype == &MarkType && m->mdata) {
		Mark *M = m->mdata;
		m->mdata = NULL;
		m->mtype = NULL;
		M->mark = NULL;
		Py_DECREF(M);
	}
	return 1;
}

static int do_Pane_init(Pane *self safe, PyObject *args, PyObject *kwds,
		       Pane **parentp safe,
		       int *zp safe)
{
	int ret;
	static const char *keywords[] = {"parent", "z", NULL};

	if (self->pane) {
		PyErr_SetString(PyExc_TypeError, "Pane already initialised");
		return -1;
	}
	if (in_pane_frompane)
		/* An internal Pane_Frompane call - it will set .pane,
		 * and we don't want a .handler.
		 */
		return 0;

	/* Pane(parent=None, z=0) */
	ret = PyArg_ParseTupleAndKeywords(args, kwds, "O!|i", (char**)keywords,
					  &PaneType, parentp, zp);
	if (ret <= 0)
		return -1;

	self->map = key_alloc();
	key_add(self->map, "Close:mark", &python_close_mark);
	self->cmd = python_pane_call;
	self->cmd.closed_ok = 1;
	self->cmd.free = python_pane_free;
	if (self->ob_base.ob_type)
		self->cmd.name = self->ob_base.ob_type->tp_name;

	return 1;
}

static int Pane_init(Pane *self safe, PyObject *args, PyObject *kwds)
{
	Pane *parent = NULL;
	int z = 0;
	int ret = do_Pane_init(self, args, kwds, &parent, &z);

	if (ret <= 0)
		return ret;
	if (!parent || !parent->pane)
		return -1;

	/* The pane holds a reference to the Pane through the ->handle
	 * function
	 */
	Py_INCREF(self);
	self->pane = pane_register(parent->pane, z, &self->cmd, self);
	if (!self->pane)
		return -1;

	pane_get(self->pane);
	return 0;
}

static int Doc_init(Doc *self, PyObject *args, PyObject *kwds)
{
	Pane *parent = NULL;
	struct python_doc *pd;
	int z = 0;
	int ret = do_Pane_init((Pane*safe)self, args, kwds, &parent, &z);

	if (ret <= 0)
		return ret;
	if (!self || !parent || !parent->pane)
		return -1;

	self->cmd.func = python_doc_call_func;
	self->pane = doc_register(parent->pane, &self->cmd);
	if (self->pane) {
		pane_get(self->pane);
		pd = self->pane->doc_data;
		pd->pdoc = self;
		pd->doc.refcnt = mark_refcnt;
	}
	return 0;
}

static inline void do_free(PyObject *ob safe)
{
	if (ob->ob_type && ob->ob_type->tp_free)
		ob->ob_type->tp_free(ob);
}

DEF_CMD(python_null_call)
{
	return 1;
}

static void python_pane_free_final(struct command *c safe)
{
	Pane *p = container_of(c, Pane, cmd);

	if (p->pane)
		pane_put(p->pane);
	python_pane_free(c);
	do_free((PyObject*safe)p);
}

static void pane_dealloc(Pane *self safe)
{
	struct pane *p = self->pane;

	/* if initialization failed, then dealloc happens before the
	 * pane gets closed.  In that case we need to modify the
	 * free sequence so do_free() gets called after the close.
	 */

	if (p && p->handle && p->handle->func == python_pane_call.func) {
		p->handle = &python_null_call;
		p->handle->free = python_pane_free_final;
		pane_close(p);
	} else if (p && p->handle && p->handle->func == python_doc_call.func) {
		p->handle = &python_null_call;
		p->handle->free = python_pane_free_final;
		pane_close(p);
	} else {
		if (p)
			pane_put(p);
		do_free((PyObject*safe)self);
	}
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

static PyObject *Pane_take_focus(Pane *self safe, PyObject *args)
{
	if (!pane_valid(self))
		return NULL;

	pane_take_focus(self->pane);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_has_focus(Pane *self safe, PyObject *args)
{
	Pane *other = NULL;
	int ret;

	if (!pane_valid(self))
		return NULL;

	ret = PyArg_ParseTuple(args, "|O!", &PaneType, &other);
	if (ret <= 0)
		return NULL;

	if (pane_has_focus(self->pane, other ? other->pane : NULL)) {
		Py_INCREF(Py_True);
		return Py_True;
	} else {
		Py_INCREF(Py_False);
		return Py_False;
	}
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
	bool return_char;
};

DEF_CB(take_focus)
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

DEF_CB(take_mark)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!mark_valid(ci->mark))
		return Efallthrough;
	if (ci->mark->viewnum == MARK_UNGROUPED) {
		/* Cannot rely on this mark persisting, take a copy */
		struct mark *m = mark_dup(ci->mark);
		pr->ret = Mark_Frommark(m);
		m->mtype = (void*)pr->ret;
	} else
		pr->ret = Mark_Frommark(ci->mark);
	return 1;
}

DEF_CB(take_mark2)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!mark_valid(ci->mark2))
		return Efallthrough;
	pr->ret = Mark_Frommark(ci->mark2);
	return 1;
}

DEF_CB(take_2_marks)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);
	PyObject *m1, *m2;

	if (pr->ret)
		return Einval;
	if (mark_valid(ci->mark))
		m1 = Mark_Frommark(ci->mark);
	else
		m1 = Py_None;
	if (mark_valid(ci->mark2))
		m2 = Mark_Frommark(ci->mark2);
	else
		m2 = Py_None;

	pr->ret = Py_BuildValue("OO", m1, m2);
	if (mark_valid(ci->mark))
		Py_DECREF(m1);
	if (mark_valid(ci->mark2))
		Py_DECREF(m2);
	return 1;
}

DEF_CB(take_str)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!ci->str)
		return Efallthrough;
	pr->ret = python_string(ci->str, -1);
	return 1;
}

DEF_CB(take_bytes)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!ci->str)
		return Efallthrough;
	pr->ret = safe_cast PyBytes_FromStringAndSize(ci->str, ci->num);
	return 1;
}

DEF_CB(take_comm)
{
	struct pyret *pr = container_of(ci->comm, struct pyret, comm);

	if (pr->ret)
		return Einval;
	if (!ci->comm2)
		return Efallthrough;
	pr->ret = Comm_Fromcomm(ci->comm2);
	return 1;
}

static struct command *map_ret(char *ret safe)
{
	if (strcmp(ret, "pane") == 0)
		return &take_focus;
	if (strcmp(ret, "mark") == 0)
		return &take_mark;
	if (strcmp(ret, "mark2") == 0)
		return &take_mark2;
	if (strcmp(ret, "marks") == 0)
		return &take_2_marks;
	if (strcmp(ret, "str") == 0)
		return &take_str;
	if (strcmp(ret, "bytes") == 0)
		return &take_bytes;
	if (strcmp(ret, "comm") == 0)
		return &take_comm;
	return NULL;
}

static bool handle_ret(PyObject *kwds, struct cmd_info *ci safe,
		       struct pyret *pr safe)
{
	char *rets;
	struct command *c;
	PyObject *ret, *s3 = NULL;

	memset(pr, 0, sizeof(*pr));

	ret = kwds ? PyDict_GetItemString(kwds, "ret") : NULL;
	if (!ret)
		return True;

	if (!PyUnicode_Check(ret) ||
	    (rets = python_as_string(ret, &s3)) == NULL) {
		PyErr_SetString(PyExc_TypeError, "ret= must be given a string");
		return False;
	}
	if (strcmp(rets, "char") == 0) {
		pr->return_char = 1;
		ret = NULL;
	} else {
		if (ci->comm2) {
			PyErr_SetString(PyExc_TypeError, "ret= not permitted with comm2");
			Py_XDECREF(s3);
			return False;
		}
		c = map_ret(rets);
		if (!c) {
			PyErr_SetString(PyExc_TypeError, "ret= type not valid");
			Py_XDECREF(s3);
			return False;
		}
		pr->comm = *c;
		ci->comm2 = &pr->comm;
	}
	Py_XDECREF(s3);
	return True;
}

static void set_err(int rv)
{
	switch(rv) {
	case Enoarg:
		PyErr_SetObject(Edlib_CommandFailed,
				PyUnicode_FromFormat("Enoarg"));
		break;
	case Einval:
		PyErr_SetObject(Edlib_CommandFailed,
				PyUnicode_FromFormat("Einval"));
		break;
	case Enosup:
		PyErr_SetObject(Edlib_CommandFailed,
				PyUnicode_FromFormat("Enosup"));
		break;
	case Efail:
		PyErr_SetObject(Edlib_CommandFailed,
				PyUnicode_FromFormat("Efail"));
		break;
	default:
		PyErr_SetObject(Edlib_CommandFailed,
				PyUnicode_FromFormat("%d", rv));
	}
}

static PyObject *choose_ret(int rv, struct pyret *pr safe)
{
	if (pr->comm.func && rv >= Efalse) {
		if (pr->ret)
			return pr->ret;
		Py_INCREF(Py_None);
		return Py_None;
	}
	Py_XDECREF(pr->ret);
	if (rv < Efalse) {
		set_err(rv);
		return NULL;
	}
	if (pr->return_char) {
		if (rv == 0) {
			Py_INCREF(Py_False);
			return Py_False;
		}
		if (rv == CHAR_RET(WEOF)) {
			Py_INCREF(Py_None);
			return Py_None;
		}
		return PyUnicode_FromFormat("%c", rv & 0x1FFFFF);
	}
	return PyLong_FromLong(rv);
}

static PyObject *Pane_call(Pane *self safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;
	struct pyret pr;

	if (!pane_valid(self))
		return NULL;

	ci.home = self->pane;

	if (!get_cmd_info(&ci, args, kwds, &s1, &s2) ||
	    !handle_ret(kwds, &ci, &pr)) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}

	python_running = False;
	rv = key_handle(&ci);
	python_running = True;

	/* Just in case ... */
	PyErr_Clear();

	Py_XDECREF(s1); Py_XDECREF(s2);
	command_put(ci.comm2);

	return choose_ret(rv, &pr);
}

static PyObject *pane_direct_call(Pane *self safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;
	struct pyret pr;

	if (!pane_valid(self))
		return NULL;

	ci.home = self->pane;

	if (!get_cmd_info(&ci, args, kwds, &s1, &s2) ||
	    !handle_ret(kwds, &ci, &pr)) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}

	ci.comm = ci.home->handle;
	rv = ci.comm->func(&ci);

	Py_XDECREF(s1); Py_XDECREF(s2);
	command_put(ci.comm2);
	return choose_ret(rv, &pr);
}

static PyObject *Pane_notify(Pane *self safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;

	if (!pane_valid(self))
		return NULL;

	ci.home = self->pane;

	if (!get_cmd_info(&ci, args, kwds, &s1, &s2)) {
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
		set_err(rv);
		return NULL;
	}
	return PyLong_FromLong(rv);
}

static PyObject *Pane_mapxy(Pane *self safe, PyObject *args)
{
	short x,y;
	struct xy xy;
	Pane *other = NULL;

	int ret = PyArg_ParseTuple(args, "O!hh", &PaneType, &other, &x, &y);
	if (ret <= 0 || !self->pane || !other || !other->pane)
		return NULL;

	xy = pane_mapxy(other->pane, self->pane, x, y, False);
	return Py_BuildValue("ii", xy.x, xy.y);
}

static PyObject *Pane_clipxy(Pane *self safe, PyObject *args)
{
	short x,y;
	struct xy xy;
	Pane *other = NULL;

	int ret = PyArg_ParseTuple(args, "O!hh", &PaneType, &other, &x, &y);
	if (ret <= 0 || !self->pane || !other || !other->pane)
		return NULL;

	xy = pane_mapxy(other->pane, self->pane, x, y, True);
	return Py_BuildValue("ii", xy.x, xy.y);
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

static PyObject *Pane_damaged(Pane *self safe, PyObject *args)
{
	int damage = DAMAGED_REFRESH;
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

static PyObject *Pane_set_time(Pane *self safe, PyObject *args)
{
	struct pane *p = self->pane;
	if (p)
		pane_set_time(p);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_too_long(Pane *self safe, PyObject *args)
{
	struct pane *p = self->pane;

	if (!p || pane_too_long(p, 0)) {
		Py_INCREF(Py_True);
		return Py_True;
	} else {
		Py_INCREF(Py_False);
		return Py_False;
	}
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
	int tostart = 0;
	int ret = PyArg_ParseTuple(args, "iO!O!|i", &view, &MarkType, &start,
				   &MarkType, &end, &tostart);

	if (ret > 0 && start && end && self->pane &&
	    start->mark && end->mark && view >= 0)
		marks_clip(self->pane, start->mark, end->mark, view, self->pane,
			   !!tostart);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_reparent(Pane *self safe, PyObject *args)
{
	Pane *newparent = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &PaneType, &newparent);

	if (ret > 0 && newparent && self->pane && newparent->pane) {
		pane_reparent(self->pane, newparent->pane);
		if (self->pane->parent != newparent->pane) {
			PyErr_SetString(PyExc_TypeError, "reparent failed");
			return NULL;
		}
	}
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

static PyObject *Pane_step(Pane *self safe, PyObject *args, int dir, int move)
{
	Mark *m = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &MarkType, &m);
	wint_t wch;

	if (!pane_valid(self))
		return NULL;
	if (ret <= 0 || !m || !mark_valid(m->mark)) {
		PyErr_SetString(PyExc_TypeError, "Arg must be a mark");
		return NULL;
	}

	if (move)
		wch = doc_move(self->pane, m->mark, dir);
	else
		wch = doc_pending(self->pane, m->mark, dir);
	if (wch == WEOF) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	return PyUnicode_FromFormat("%c", wch);
}

static PyObject *Pane_step_next(Pane *self safe, PyObject *args)
{
	return Pane_step(self, args, 1, 1);
}

static PyObject *Pane_step_prev(Pane *self safe, PyObject *args)
{
	return Pane_step(self, args, -1, 1);
}

static PyObject *Pane_step_following(Pane *self safe, PyObject *args)
{
	return Pane_step(self, args, 1, 0);
}

static PyObject *Pane_step_prior(Pane *self safe, PyObject *args)
{
	return Pane_step(self, args, -1, 0);
}

static PyObject *Pane_get_vmarks(Pane *self safe, PyObject *args)
{
	struct pyret pr;
	Pane *owner = NULL;
	int view = -1;
	int ret;

	if (!pane_valid(self))
		return NULL;
	ret = PyArg_ParseTuple(args, "i|O!", &view, &PaneType, &owner);
	if (ret <= 0 || view < 0 || (owner && !pane_valid(owner))) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	memset(&pr, 0, sizeof(pr));
	pr.comm = take_2_marks;
	home_call_comm(self->pane, "doc:vmark-get",
		       owner ? owner->pane : self->pane,
		       &pr.comm, view);
	if (pr.ret)
		return pr.ret;
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Pane_vmark_at_or_before(Pane *self safe, PyObject *args)
{
	Mark *m = NULL;
	Pane *owner = NULL;
	struct pyret pr;
	int view = -1;
	int ret;

	if (!pane_valid(self))
		return NULL;
	ret = PyArg_ParseTuple(args, "iO!|O!", &view, &MarkType, &m,
			       &PaneType, &owner);
	if (ret <= 0 || view < 0 || !m || !mark_valid(m->mark) ||
	    (owner && !pane_valid(owner))) {
		Py_INCREF(Py_None);
		return Py_None;
	}
	memset(&pr, 0, sizeof(pr));
	pr.comm = take_mark;
	home_call_comm(self->pane, "doc:vmark-prev",
		       owner ? owner->pane : self->pane,
		       &pr.comm, view, m->mark);
	if (pr.ret)
		return pr.ret;
	Py_INCREF(Py_None);
	return Py_None;
}

static const PyMethodDef pane_methods[] = {
	{"close", (PyCFunction)Pane_close, METH_NOARGS,
	 "close the pane"},
	{"children", (PyCFunction)pane_children, METH_NOARGS,
	 "provides an iterator which will iterate over all children"},
	{"clone_children", (PyCFunction)Pane_clone_children, METH_VARARGS,
	 "Clone all children onto the target"},
	{"take_focus", (PyCFunction)Pane_take_focus, METH_NOARGS,
	 "Claim the focus for this pane"},
	{"has_focus", (PyCFunction)Pane_has_focus, METH_VARARGS,
	 "Check if pane is focus of display"},
	{"call", (void*)(PyCFunctionWithKeywords)Pane_call, METH_VARARGS|METH_KEYWORDS,
	 "Call a command from a pane"},
	{"notify", (void*)(PyCFunctionWithKeywords)Pane_notify, METH_VARARGS|METH_KEYWORDS,
	 "Send a notification from a pane"},
	{"mapxy", (PyCFunction)Pane_mapxy, METH_VARARGS,
	 "Convert pane-relative co-ords between panes"},
	{"clipxy", (PyCFunction)Pane_clipxy, METH_VARARGS,
	 "Convert pane-relative co-ords between panes, clipping to all panes"},
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
	{"next", (PyCFunction)Pane_step_next, METH_VARARGS,
	 "Move mark forward returning the character"},
	{"prev", (PyCFunction)Pane_step_prev, METH_VARARGS,
	 "Move mark back returning the character"},
	{"following", (PyCFunction)Pane_step_following, METH_VARARGS,
	 "returning the character after mark"},
	{"prior", (PyCFunction)Pane_step_prior, METH_VARARGS,
	 "returning the character before mark"},
	{"vmarks", (PyCFunction)Pane_get_vmarks, METH_VARARGS,
	 "return first and last vmark given view number"},
	{"vmark_at_or_before", (PyCFunction)Pane_vmark_at_or_before, METH_VARARGS,
	 "return vmark at-or-before given mark"},
	{"set_time", (PyCFunction)Pane_set_time, METH_NOARGS,
	 "Set start time for long running operation"},
	{"too_long", (PyCFunction)Pane_too_long, METH_NOARGS,
	 "Check if command in pane has been running for too long"},
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
	if (*which == 'F')
		new = pane_focus(p->pane);
	if (*which == 'L')
		new = pane_leaf(p->pane);
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

static PyObject *pane_repr(Pane *self safe)
{
	char *s = NULL;
	PyObject *ret;
	if (!pane_valid(self))
		asprintf(&s, "<edlib.Pane FREED!!! %p>", self);
	else
		asprintf(&s, "<edlib.Pane %p-%s>", self->pane, self->pane->name);
	ret = Py_BuildValue("s", s);
	free(s);
	return ret;
}

static PyObject *doc_repr(Doc *self safe)
{
	char *s = NULL;
	PyObject *ret;
	if (!doc_valid(self))
		asprintf(&s, "<edlib.Doc FREED!!! %p>", self);
	else
		asprintf(&s, "<edlib.Doc %p-%s>", self->pane, self->pane->name);
	ret = Py_BuildValue("s", s);
	free(s);
	return ret;
}

static long pane_hash(Pane *p safe)
{
	return (long)p->pane;
}

static PyObject *pane_cmp(Pane *p1 safe, Pane *p2 safe, int op)
{
	Py_RETURN_RICHCOMPARE(p1->pane, p2->pane, op);
}

static const PyGetSetDef pane_getseters[] = {
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
	 "Root pane", "r"},
	{"final_focus",
	 (getter)pane_getpane, (setter)pane_nosetpane,
	 "Final focus pane", "F"},
	{"leaf",
	 (getter)pane_getpane, (setter)pane_nosetpane,
	 "Leaf pane", "L"},
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

static const PyMappingMethods pane_mapping = {
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
	.tp_as_mapping	= (PyMappingMethods*)&pane_mapping,
	.tp_hash	= (hashfunc)pane_hash,
	.tp_call	= (ternaryfunc)pane_direct_call,
	.tp_flags	= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc		= "edlib panes",
	.tp_methods	= (PyMethodDef*)pane_methods,
	.tp_getset	= (PyGetSetDef*)pane_getseters,
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

	m = mark_first(&self->pane->doc_data->doc);
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

	ret = PyArg_ParseTuple(args, "O!p", &MarkType, &mark, &end);
	if (ret <= 0 || !mark || !mark_valid(mark->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark undefined or uninitialized");
		return NULL;
	}

	mark_to_end(self->pane, mark->mark, end);
	Py_INCREF(Py_None);
	return Py_None;
}

static const PyMethodDef doc_methods[] = {
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
	.tp_methods	= (PyMethodDef*)doc_methods,
	.tp_base	= &PaneType,
	.tp_init	= (initproc)Doc_init,
	.tp_new		= (newfunc)Doc_new,
};

static PyObject *mark_getoffset(Mark *m safe, void *x)
{
	struct doc *d;
	if (!mark_valid(m->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	d = &m->mark->owner->doc;
	if (d->refcnt == mark_refcnt)
		return PyLong_FromLong(m->mark->ref.o);
	return PyLong_FromLong(0);
}

static int mark_setoffset(Mark *m safe, PyObject *v safe, void *x)
{
	struct doc *d;
	long val;

	if (!mark_valid(m->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	val = PyLong_AsLong(v);
	if (val == -1 && PyErr_Occurred())
		return -1;
	d = &m->mark->owner->doc;
	if (d->refcnt == mark_refcnt)
		m->mark->ref.o = val;
	else {
		PyErr_SetString(PyExc_TypeError, "Setting offset on non-local mark");
		return -1;
	}
	return 0;
}

static PyObject *mark_getseq(Mark *m safe, void *x)
{
	if (!mark_valid(m->mark)) {
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
	struct doc *d;
	if (!mark_valid(m->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	d = &m->mark->owner->doc;
	if (d->refcnt == mark_refcnt && m->mark->ref.c) {
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
	struct doc *d;

	if (!mark_valid(m->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return -1;
	}
	d = &m->mark->owner->doc;
	if (d->refcnt != mark_refcnt) {
		PyErr_SetString(PyExc_TypeError, "Cannot set ref for non-local mark");
		return -1;
	}
	d->refcnt(m->mark, -1);
	if (v == Py_None)
		v = NULL;
	/* If an adjacent mark has a ref.c with a matching value
	 * use that instead, so that mark_same() works.
	 */
	if ((m2 = mark_next(m->mark)) != NULL &&
	    m2->owner->doc.refcnt == mark_refcnt &&
	    m2->ref.c != NULL && v != NULL &&
	    PyObject_RichCompareBool(v, m2->ref.c, Py_EQ) == 1)
		m->mark->ref.c = m2->ref.c;
	else if ((m2 = mark_prev(m->mark)) != NULL &&
		 m2->owner->doc.refcnt == mark_refcnt &&
		 m2->ref.c != NULL && v != NULL &&
		 PyObject_RichCompareBool(v, m2->ref.c, Py_EQ) == 1)
		m->mark->ref.c = m2->ref.c;
	else
		m->mark->ref.c = v;
	d->refcnt(m->mark, 1);
	return 0;
}

static PyObject *mark_getview(Mark *m safe, void *x)
{
	if (!mark_valid(m->mark)) {
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
	} else if (!mark_valid(a->mark) || !mark_valid(b->mark))
		return NULL;
	else {
		int cmp = a->mark->seq - b->mark->seq;
		if (mark_same(a->mark, b->mark))
			cmp = 0;
		Py_RETURN_RICHCOMPARE(cmp, 0, op);
	}
}

static const PyGetSetDef mark_getseters[] = {
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

static Mark *Mark_new(PyTypeObject *type safe, PyObject *args, PyObject *kwds)
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
	static const char *keywords[] = {"pane","view","orig", "owner", NULL};
	int ret;

	if (!PyTuple_Check(args) ||
	    (PyTuple_GET_SIZE(args) == 0 && kwds == NULL))
		/* Internal Mark_Frommark call */
		return 1;

	ret = PyArg_ParseTupleAndKeywords(args, kwds, "|O!iO!O!", (char**)keywords,
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
	} else if (orig && mark_valid(orig->mark)) {
		self->mark = mark_dup_view(orig->mark);
	}
	if (!self->mark) {
		PyErr_SetString(PyExc_TypeError, "Mark creation failed");
		return -1;
	}
	if (self->mark->viewnum >= 0) {
		/* vmarks can use mdata and don't disappear until
		 * explicitly released.
		 */
		self->mark->mtype = &MarkType;
		self->mark->mdata = self;
		Py_INCREF(self);
	} else {
		/* Other marks cannot use mdata and get freed when
		 * the original PyObject is destroyed
		 */
		self->mark->mtype = (void*)self;
	}
	return 1;
}

static void mark_dealloc(Mark *self safe)
{
	if (mark_valid(self->mark) && self->mark->mtype == (void*)self) {
		/* Python allocated this mark, so can free it. */
		struct mark *m = self->mark;
		self->mark = NULL;
		m->mtype = NULL;
		m->mdata = NULL;
		mark_free(m);
	}
	do_free((PyObject*safe)self);
}

static PyObject *Mark_to_mark(Mark *self safe, PyObject *args)
{
	Mark *other = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &MarkType, &other);
	if (ret <= 0 || !other ||
	    !mark_valid(self->mark) || !mark_valid(other->mark))
		return NULL;
	mark_to_mark(self->mark, other->mark);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_to_mark_noref(Mark *self safe, PyObject *args)
{
	Mark *other = NULL;
	int ret = PyArg_ParseTuple(args, "O!", &MarkType, &other);
	if (ret <= 0 || !other ||
	    !mark_valid(self->mark) || !mark_valid(other->mark))
		return NULL;
	mark_to_mark_noref(self->mark, other->mark);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_clip(Mark *self safe, PyObject *args)
{
	Mark *start = NULL, *end = NULL;
	int tostart = 0;
	int ret = PyArg_ParseTuple(args, "O!O!|p", &MarkType, &start,
				   &MarkType, &end, &tostart);

	if (ret > 0 && start && end && mark_valid(self->mark) &&
	    mark_valid(start->mark) && mark_valid(end->mark))
		mark_clip(self->mark, start->mark, end->mark, tostart);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_step(Mark *self safe, PyObject *args)
{
	/* Convenience function to help implement doc:char */
	int forward = 1;
	int ret = PyArg_ParseTuple(args, "i", &forward);

	if (ret > 0 && self->mark)
		mark_step(self->mark, forward);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_step_sharesref(Mark *self safe, PyObject *args)
{
	/* Convenience function to help implement doc:char */
	int forward = 1;
	int ret = PyArg_ParseTuple(args, "i", &forward);

	if (ret > 0 && self->mark)
		mark_step_sharesref(self->mark, forward);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_next(Mark *self safe, PyObject *args)
{
	struct mark *next;
	if (!mark_valid(self->mark)) {
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
	if (!mark_valid(self->mark)) {
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
	if (!mark_valid(self->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	next = mark_next(self->mark);
	if (next)
		return Mark_Frommark(next);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_prev_any(Mark *self safe, PyObject *args)
{
	struct mark *prev;
	if (!mark_valid(self->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	prev = mark_prev(self->mark);
	if (prev)
		return Mark_Frommark(prev);
	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_dup(Mark *self safe, PyObject *args)
{
	struct mark *new;
	if (!mark_valid(self->mark)) {
		PyErr_SetString(PyExc_TypeError, "Mark is NULL");
		return NULL;
	}
	new = mark_dup(self->mark);
	if (new) {
		Mark *ret = (Mark*)Mark_Frommark(new);
		/* We want this mark to be freed when the Mark
		 * dies
		 */
		new->mtype = (void*)ret;
		return (PyObject*)ret;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_release(Mark *self safe, PyObject *args)
{
	struct mark *m = self->mark;

	if (!mark_valid(m)) {
		PyErr_SetString(PyExc_TypeError, "Mark has been freed");
		return NULL;
	}
	if (m->viewnum == MARK_UNGROUPED || m->viewnum == MARK_POINT) {
		PyErr_SetString(PyExc_TypeError,
				"Cannot release ungrouped marks or points");
		return NULL;
	}
	if (m->mtype != &MarkType) {
		PyErr_SetString(PyExc_TypeError,
				"Mark is not managed by python, and cannot be released");
		return NULL;
	}

	/* We are dropping this mark - there cannot be any other ref */
	ASSERT(m->mdata == self);
	self->mark = NULL;
	Py_DECREF(self);
	m->mdata = NULL;
	m->mtype = NULL;
	mark_free(m);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *Mark_watch(Mark *self safe, PyObject *args)
{
	struct mark *m = self->mark;

	if (!mark_valid(m)) {
		PyErr_SetString(PyExc_TypeError, "Mark has been freed");
		return NULL;
	}
	mark_watch(m);
	Py_INCREF(Py_None);
	return Py_None;
}

static const PyMethodDef mark_methods[] = {
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
	 "If this mark is in range, move to end"},
	{"release", (PyCFunction)Mark_release, METH_NOARGS,
	 "release a vmark so it can disappear"},
	{"watch", (PyCFunction)Mark_watch, METH_NOARGS,
	 "acknowledge movement of a point - allow further notifications"},
	{"step", (PyCFunction)Mark_step, METH_VARARGS,
	 "Move mark over any adjacent marks with same reference"},
	{"step_sharesref", (PyCFunction)Mark_step_sharesref, METH_VARARGS,
	 "Move mark over any adjacent marks with same reference"},
	{NULL}
};

static PyObject *mark_get_item(Mark *self safe, PyObject *key safe)
{
	char *k, *v;
	PyObject *t1 = NULL;

	if (!mark_valid(self->mark)) {
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
	if (!mark_valid(self->mark)) {
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

static PyObject *mark_repr(Mark *self safe)
{
	char *s = NULL, *dm;
	PyObject *ret;

	if (!self->mark)
		asprintf(&s, "<edlib.Mark NULL %p>", self);
	else if (!mark_valid(self->mark))
		asprintf(&s, "<edlib.Mark FREED %p>", self);
	else if ((dm = call_ret(str, "doc:debug:mark", self->mark->owner,
				0, self->mark))
		 != NULL)
		asprintf(&s, "<edlib.Mark seq=%d v=%d %s>",
			 self->mark->seq, self->mark->viewnum, dm);
	else
		asprintf(&s, "<edlib.Mark seq=%d v=%d i=%d %p>",
			 self->mark->seq, self->mark->viewnum,
			 self->mark->ref.o, self->mark);

	ret = Py_BuildValue("s", s);
	free(s);
	return ret;
}

static const PyMappingMethods mark_mapping = {
	.mp_length = NULL,
	.mp_subscript = (binaryfunc)mark_get_item,
	.mp_ass_subscript = (objobjargproc)mark_set_item,
};

static int mark_bool(Mark *self safe)
{
	return mark_valid(self->mark);
}

static const PyNumberMethods mark_as_num = {
	.nb_bool	= (inquiry) mark_bool,
};

static PyTypeObject MarkType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	.tp_name	= "edlib.Mark",
	.tp_basicsize	= sizeof(Mark),
	.tp_dealloc	= (destructor)mark_dealloc,
	.tp_as_mapping	= (PyMappingMethods*)&mark_mapping,
	.tp_flags	= Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
	.tp_doc		= "edlib marks",
	.tp_richcompare	= (richcmpfunc)mark_compare,
	.tp_methods	= (PyMethodDef*)mark_methods,
	.tp_getset	= (PyGetSetDef*)mark_getseters,
	.tp_init	= (initproc)Mark_init,
	.tp_new		= (newfunc)Mark_new,
	.tp_repr	= (reprfunc)mark_repr,
	.tp_as_number	= (PyNumberMethods*)&mark_as_num,
};

static void comm_dealloc(Comm *self safe)
{
	command_put(self->comm);
	do_free((PyObject*safe)self);
}

static PyObject *comm_repr(Comm *self safe)
{
	char *s = NULL;
	PyObject *ret;

	if (self->comm)
		asprintf(&s, "<edlib.Comm refcnt=%d %p>",
			 self->comm->refcnt, self->comm);
	else
		asprintf(&s, "<edlib.Comm NULL %p>", self);
	ret = Py_BuildValue("s", s);
	free(s);
	return ret;
}

static PyObject *Comm_call(Comm *c safe, PyObject *args safe, PyObject *kwds)
{
	struct cmd_info ci = SAFE_CI;
	int rv;
	PyObject *s1, *s2;
	struct pyret pr;

	if (!c->comm)
		return NULL;
	if (!get_cmd_info(&ci, args, kwds, &s1, &s2) ||
	    !handle_ret(kwds, &ci, &pr)) {
		Py_XDECREF(s1); Py_XDECREF(s2);
		command_put(ci.comm2);
		return NULL;
	}
	ci.comm = c->comm;
	rv = c->comm->func(&ci);
	Py_XDECREF(s1); Py_XDECREF(s2);
	command_put(ci.comm2);

	return choose_ret(rv, &pr);
}

static PyObject *comm_cmp(Comm *c1 safe, Comm *c2 safe, int op)
{
	Py_RETURN_RICHCOMPARE(c1->comm, c2->comm, op);
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
	.tp_richcompare	= (richcmpfunc)comm_cmp,
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
	list_del(&pc->lst);
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
static bool get_cmd_info(struct cmd_info *ci safe, PyObject *args safe, PyObject *kwds,
			 PyObject **s1 safe, PyObject **s2 safe)
{
	int argc;
	PyObject *a;
	int i;
	int num_set = 0, num2_set = 0;
	int xy_set = 0;

	*s1 = *s2 = NULL;

	if (!PyTuple_Check(args))
		return False;
	argc = PyTuple_GET_SIZE(args);
	if (argc >= 1) {
		/* First positional arg must be the key */
		a = PyTuple_GetItem(args, 0);
		if (!PyUnicode_Check(a)) {
			PyErr_SetString(PyExc_TypeError, "First arg must be key");
			return False;
		}
		ci->key = safe_cast PyUnicode_AsUTF8(a);
	}
	for (i = 1; i < argc; i++) {
		a = PyTuple_GetItem(args, i);
		if (!a || a == Py_None)
			/* quietly ignore */;
		else if (PyObject_TypeCheck(a, &PaneType)) {
			if ((void*)ci->home == NULL)
				ci->home = safe_cast ((Pane*)a)->pane;
			else if ((void*)ci->focus == NULL)
				ci->focus = safe_cast ((Pane*)a)->pane;
			else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Pane args permitted");
				return False;
			}
		} else if (PyObject_TypeCheck(a, &MarkType)) {
			if (ci->mark == NULL)
				ci->mark = ((Mark*)a)->mark;
			else if (ci->mark2 == NULL)
				ci->mark2 = ((Mark*)a)->mark;
			else {
				PyErr_SetString(PyExc_TypeError, "Only 2 Mark args permitted");
				return False;
			}
		} else if (PyUnicode_Check(a)) {
			char *str;
			PyObject *s = NULL;
			if (ci->str && ci->str2) {
				PyErr_SetString(PyExc_TypeError, "Only 3 String args permitted");
				return False;
			}
			str = python_as_string(a, &s);
			if (!str)
				return False;
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
				return False;
			}
		} else if (PyTuple_Check(a)) {
			int n = PyTuple_GET_SIZE(a);
			PyObject *n1, *n2;
			if (n != 2) {
				PyErr_SetString(PyExc_TypeError, "Only 2-element tuples permitted");
				return False;
			}
			n1 = PyTuple_GetItem(a, 0);
			n2 = PyTuple_GetItem(a, 1);
			if (!PyLong_Check(n1) || !PyLong_Check(n2)) {
				PyErr_SetString(PyExc_TypeError, "Only tuples of integers permitted");
				return False;
			}
			if (!xy_set) {
				ci->x = PyLong_AsLong(n1);
				ci->y = PyLong_AsLong(n2);
				xy_set = 1;
			} else {
				PyErr_SetString(PyExc_TypeError, "Only one tuple permitted");
				return False;
			}
		} else if (PyObject_TypeCheck(a, &CommType)) {
			Comm *c = (Comm*)a;
			if (ci->comm2 == NULL && c->comm) {
				ci->comm2 = command_get(c->comm);
			} else {
				PyErr_SetString(PyExc_TypeError, "Only one callable permitted");
				return False;
			}
		} else if (PyCallable_Check(a)) {
			struct python_command *pc = export_callable(a, NULL);

			if (ci->comm2 == NULL)
				ci->comm2 = &pc->c;
			else {
				command_put(&pc->c);
				PyErr_SetString(PyExc_TypeError, "Only one callable permitted");
				return False;
			}
		} else {
			PyErr_Format(PyExc_TypeError, "Unsupported arg type %d", i);
			return False;
		}
	}
	if (kwds && PyDict_Check(kwds)) {
		a = PyDict_GetItemString(kwds, "str1");
		if (!a || a == Py_None)
			a = PyDict_GetItemString(kwds, "str");
		if (a && a != Py_None) {
			if (*s1 || ci->str) {
				PyErr_SetString(PyExc_TypeError,
						"'str' given with other strings");
				return False;
			}
			if (!PyUnicode_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'str' must be string or unicode");
				return False;
			}
			ci->str = python_as_string(a, s1);
		}
		a = PyDict_GetItemString(kwds, "str2");
		if (a && a != Py_None) {
			if (*s2 || ci->str2) {
				PyErr_SetString(PyExc_TypeError,
						"'str2' given with 2 strings");
				return False;
			}
			if (!PyUnicode_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'str2' must be string or unicode");
				return False;
			}
			ci->str2 = python_as_string(a, s2);
		}
		a = PyDict_GetItemString(kwds, "mark");
		if (a && a != Py_None) {
			if (ci->mark) {
				PyErr_SetString(PyExc_TypeError,
						"'mark' given with other marks");
				return False;
			}
			if (!PyObject_TypeCheck(a, &MarkType)) {
				PyErr_SetString(PyExc_TypeError,
						"'mark' must be an edlib.Mark");
				return False;
			}
			ci->mark = ((Mark*)a)->mark;
		}
		a = PyDict_GetItemString(kwds, "mark2");
		if (a && a != Py_None) {
			if (ci->mark2) {
				PyErr_SetString(PyExc_TypeError,
						"'mark2' given with 2 other marks");
				return False;
			}
			if (!PyObject_TypeCheck(a, &MarkType)) {
				PyErr_SetString(PyExc_TypeError,
						"'mark2' must be an edlib.Mark");
				return False;
			}
			ci->mark2 = ((Mark*)a)->mark;
		}
		a = PyDict_GetItemString(kwds, "num");
		if (a) {
			if (num_set) {
				PyErr_SetString(PyExc_TypeError,
						"'num' given with other numbers");
				return False;
			}
			if (!PyLong_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'num' must be an integer");
				return False;
			}
			ci->num = PyLong_AsLong(a);
			num_set = 1;
		}
		a = PyDict_GetItemString(kwds, "num2");
		if (a) {
			if (num2_set) {
				PyErr_SetString(PyExc_TypeError,
						"'num2' given with 2 other numbers");
				return False;
			}
			if (!PyLong_Check(a)) {
				PyErr_SetString(PyExc_TypeError,
						"'num2' must be an integer");
				return False;
			}
			ci->num2 = PyLong_AsLong(a);
			num2_set = 1;
		}
		a = PyDict_GetItemString(kwds, "focus");
		if (a && a != Py_None) {
			Pane *p;
			if ((void*)ci->focus) {
				PyErr_SetString(PyExc_TypeError,
						"'focus' given with other pane");
				return False;
			}
			if (!PyObject_TypeCheck(a, &PaneType)) {
				PyErr_SetString(PyExc_TypeError,
						"'focus' must be a pane");
				return False;
			}
			p = (Pane*)a;
			if (p->pane)
				ci->focus = p->pane;
			else {
				PyErr_SetString(PyExc_TypeError, "focus value invalid");
				return False;
			}
		}
		a = PyDict_GetItemString(kwds, "xy");
		if (a && a != Py_None) {
			PyObject *n1, *n2;
			if (xy_set) {
				PyErr_SetString(PyExc_TypeError,
						"'xy' given with other tuple");
				return False;
			}
			if (!PyTuple_Check(a) || PyTuple_GET_SIZE(a) != 2) {
				PyErr_SetString(PyExc_TypeError,
						"'xy' must be a tuple of 2 integers");
				return False;
			}
			n1 = PyTuple_GetItem(a, 0);
			n2 = PyTuple_GetItem(a, 1);
			if (!PyLong_Check(n1) || !PyLong_Check(n2)) {
				PyErr_SetString(PyExc_TypeError, "Only tuples of integers permitted");
				return False;
			}
			ci->x = PyLong_AsLong(n1);
			ci->y = PyLong_AsLong(n2);
			xy_set = 1;
		}
		a = PyDict_GetItemString(kwds, "comm2");
		if (a && a != Py_None) {
			if (ci->comm2) {
				PyErr_SetString(PyExc_TypeError,
						"'comm2' given with other command");
				return False;
			}
			if (PyObject_TypeCheck(a, &CommType)) {
				Comm *c = (Comm*)a;
				if (c->comm)
					ci->comm2 = command_get(c->comm);
				else {
					PyErr_SetString(PyExc_TypeError, "comm2 value invalid");
					return False;
				}
			} else if (PyCallable_Check(a)) {
				struct python_command *pc = export_callable(a, NULL);

				ci->comm2 = &pc->c;
			} else {
				PyErr_SetString(PyExc_TypeError,
						"'comm2' must be a callable");
				return False;
			}
		}
	}
	if (!(void*)ci->key) {
		PyErr_SetString(PyExc_TypeError, "No key specified");
		return False;
	}
	if (!(void*)ci->home) {
		PyErr_SetString(PyExc_TypeError, "No pane specified");
		return False;
	}
	if (!(void*)ci->focus)
		ci->focus = ci->home;

	return True;
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

static PyObject *py_LOG(PyObject *self, PyObject *args)
{
	int argc = PySequence_Length(args);
	int i;
	char buf[1024];
	int l = 0;

	for (i = 0; i < argc && l < (int)sizeof(buf) - 2; i++) {
		PyObject *o = PySequence_GetItem(args, i);
		PyObject *s, *tofree = NULL;
		char *str;
		unsigned int slen;

		if (!o)
			continue;
		s = PyObject_Str(o);
		Py_DECREF(o);
		if (!s)
			continue;
		str = python_as_string(s, &tofree);
		slen = str ? strlen(str) : 0;
		if (slen + l + 2 > sizeof(buf))
			slen = sizeof(buf) - l - 2;
		if (str) {
			if (l)
				buf[l++] = ' ';
			strncpy(buf+l, str, slen);
			l += slen;
		}
		Py_XDECREF(tofree);
	}
	buf[l] = 0;
	if (l)
		LOG("%s", buf);

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *py_LOG_BT(PyObject *self, PyObject *args)
{
	LOG_BT();
	Py_INCREF(Py_None);
	return Py_None;
}

static const PyMethodDef edlib_methods[] = {
	{"time_start", py_time_start, METH_VARARGS,
	 "Record start time"},
	{"time_stop", py_time_stop, METH_VARARGS,
	 "Record stop time"},
	{"LOG", py_LOG, METH_VARARGS,
	 "Generate log message"},
	{"LOG_BT", py_LOG_BT, METH_NOARGS,
	 "Generate backtrace message"},
	{NULL, NULL, 0, NULL}
};

/* This must be visible when the module is loaded so it
 * cannot be static.  sparse doesn't like variables that are
 * neither extern nor static.  So mark it extern
 */
extern char *edlib_module_path;
char *edlib_module_path;

void edlib_init(struct pane *ed safe)
{
	PyObject *m;
	PyConfig config;
	char *argv[2]= { "edlib", NULL };
	sighandler_t sigint;

	if (edlib_module_path)
		module_dir = strdup(edlib_module_path);
	else
		module_dir = ".";

	/* de-register SIGINT while we initialise python,
	 * so that Python thinks that it's registration is
	 * valid - so PyErr_SetInterrupt() don't think there
	 * is a race.
	 */
	sigint = signal(SIGINT, SIG_DFL);
	PyConfig_InitPythonConfig(&config);
	config.isolated = 1;
	PyConfig_SetBytesArgv(&config, 0, argv);
	Py_InitializeFromConfig(&config);
	PyConfig_Clear(&config);
	signal(SIGINT, sigint);

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

	m = PyImport_AddModule("edlib");
	if (!m) {
		PyErr_LOG();
		return;
	}

	PyModule_SetDocString(m , "edlib - one more editor is never enough");

	PyModule_AddFunctions(m, (PyMethodDef*)edlib_methods);
	PyModule_AddObject(m, "editor", Pane_Frompane(ed));
	PyModule_AddObject(m, "Pane", (PyObject *)&PaneType);
	PyModule_AddObject(m, "PaneIter", (PyObject *)&PaneIterType);
	PyModule_AddObject(m, "Mark", (PyObject *)&MarkType);
	PyModule_AddObject(m, "Comm", (PyObject *)&CommType);
	PyModule_AddObject(m, "Doc", (PyObject *)&DocType);
	PyModule_AddIntMacro(m, DAMAGED_CHILD);
	PyModule_AddIntMacro(m, DAMAGED_SIZE);
	PyModule_AddIntMacro(m, DAMAGED_VIEW);
	PyModule_AddIntMacro(m, DAMAGED_REFRESH);
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
	PyModule_AddIntMacro(m, TIME_MISC);

	PyModule_AddIntMacro(m, RXLF_ANCHORED);
	PyModule_AddIntMacro(m, RXLF_BACKTRACK);

	PyModule_AddIntMacro(m, MARK_UNGROUPED);
	PyModule_AddIntMacro(m, MARK_POINT);

	PyModule_AddIntConstant(m, "WEOF", 0x3FFFFF);

	PyModule_AddIntConstant(m, "testing", edlib_testing(ed));

	Edlib_CommandFailed = PyErr_NewException("edlib.commandfailed", NULL, NULL);
	Py_INCREF(Edlib_CommandFailed);
	PyModule_AddObject(m, "commandfailed", Edlib_CommandFailed);

	EdlibModule = m;
	ed_pane = ed;

	call_comm("global-set-command", ed, &handle_alarm,
		  0, NULL, "fast-alarm-python");

	call_comm("global-set-command", ed, &python_load_module,
		  0, NULL, "global-load-modules:python");
}
