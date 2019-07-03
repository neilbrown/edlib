# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2018 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

class CModePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.paren_start = None
        self.paren_end = None
        self.spaces = None   # is set to a number, use spaces, else TABs

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = CModePane(focus)
        self.clone_children(p)
        return 1

    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        # If there is white space before or after the cursor,
        # remove it.  Then work out how indented this line
        # should be, based on last non-empty line, and insert
        # that much space.
        m = mark.dup()
        c = focus.call("doc:step", 1, 0, m, ret="char")
        while c and c in " \t":
	        focus.call("doc:step", 1, 1, m)
	        c = focus.call("doc:step", 1, 0, m, ret="char")
	focus.call("Move-to", m)
        c = focus.call("doc:step", 0, 1, m, ret="char")
        while c != None:
            if c not in " \t":
                focus.call("doc:step", 1, 1, m)
                break
            c = focus.call("doc:step", 0, 1, m, ret="char")
        indent_end = m.dup()
        new = self.find_indent(focus, indent_end)
        extra = self.find_extra_indent(focus, m, indent_end)

        return focus.call("Replace", 1, m, "\n" + new + extra)

    def find_indent(self, focus, m):
        # Find previous line which is not empty and return
        # a string containing the leading tabs/spaces.
        # The mark is moved to the end of the indent.
        while focus.call("doc:step", 0, m, ret="char") == '\n':
            focus.call("doc:step", 0, 1, m)
        if focus.call("doc:step", 0, m) == edlib.WEOF:
            return ""
        # line before m is not empty
        m2 = m.dup()
        c = focus.call("doc:step", 0, m2, ret="char")
        while c != None and c != '\n':
            focus.call("doc:step", 0, 1, m2)
            if c not in " \t":
                m.to_mark(m2)
            c = focus.call("doc:step", 0, m2, ret="char")
        # m2 .. m is the prefix
        return focus.call("doc:get-str", m2, m, ret = 'str')

    def find_extra_indent(self, focus, m, indent_end):
        # now see if a () expression started since the indent.
        expr = m.dup()
        focus.call("Move-Expr", -1, 1, expr)
        if expr > indent_end:
            focus.call("doc:step", expr, 1, 1)
            if focus.call("doc:step", expr, 1, 0, ret="char") == '\n':
                # open-bracket at end-of-line, so add a standard indent
                if self.spaces:
                    extra = ' ' * self.spaces
                else:
                    extra = "\t"
            else:
                extra = focus.call("doc:get-str", indent_end, expr, ret='str')
                extra = ' ' * len(extra)
        else:
                extra = ""
        return extra

    def handle_replace(self, key, focus, **a):
	"handle:Replace"
	if self.paren_end:
	    focus.call("doc:step", self.paren_end, 1, 1)
	    focus.call("Notify:change", self.paren_start, self.paren_end)
	self.paren_start = None
	self.paren_end = None
	return 0

    def handle_refresh(self, key, focus, **a):
	"handle:Refresh"
	point = focus.call("doc:point", ret = 'mark')
	if self.paren_end:
	    # maybe marks are still OK
	    m = point.dup()
	    if focus.call("doc:step", m, 0, 1, ret='char') and m == self.paren_end:
		return 0
	    focus.call("doc:step", self.paren_end, 1, 1)
	    focus.call("Notify:change", self.paren_start, self.paren_end)
	    self.paren_end = None
	    self.paren_start = None

	c = focus.call("doc:step", point, 0, 0, ret = 'char')
	if c and c in ')}]':
		m = point.dup()
		focus.call("doc:step", m, 0, 1)
		self.paren_end = m
		m['render:paren'] = "close"
		m = point.dup()
		focus.call("Move-Expr", m, -1)
		m['render:paren'] = "open"
		self.paren_start = m
		focus.call("Notify:change", self.paren_start, point)
	return 0

    def handle_map_attr(self, key, focus, str, str2, comm2, **a):
	"handle:map-attr"
	if str == "render:paren":
		comm2("cb", focus, "bg:pink,bold", 1)

def c_mode_attach(key, focus, comm2, **a):
    p = focus.render_attach("text")
    p = CModePane(p)
    comm2("callback", p)
    return 1

def py_mode_attach(key, focus, comm2, **a):
    p = focus.render_attach("text")
    p = CModePane(p)
    p.spaces = 4
    comm2("callback", p)
    return 1

def c_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-2:] in [".c", ".h"]:
        focus["render-default"] = "c-mode"
    return 0

def py_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-3:] in [".py"]:
        focus["render-default"] = "py-mode"
    return 0

def attach_indent(key, focus, **a):
    CModePane(focus)
    return 1

editor.call("global-set-command", "doc:appeared-c-mode", c_mode_appeared)
editor.call("global-set-command", "doc:appeared-py-mode", py_mode_appeared)
editor.call("global-set-command", "attach-render-c-mode", c_mode_attach)
editor.call("global-set-command", "attach-render-py-mode", py_mode_attach)
editor.call("global-set-command", "interactive-cmd-indent", attach_indent)
