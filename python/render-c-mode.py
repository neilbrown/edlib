# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2018 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

class CModePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        # If there is white space at the end of the line,
        # remove it.  Then work out how indented this line
        # should be, base on last non-empty line, and insert
        # that much space.
        m = mark.dup()
        c = self.call("doc:step", focus, 0, 1, m)
        while c != edlib.WEOF:
            ch = chr(c&0xfffff)
            if ch not in " \t":
                self.call("doc:step", focus, 1, 1, m)
                break
            c = self.call("doc:step", focus, 0, 1, m)
        indent_end = m.dup()
        new = self.find_indent(focus, indent_end)
        # now see if a () expression started since the indent.
        expr = m.dup()
        focus.call("Move-Expr", -1, 1, expr)
        if expr > indent_end:
            focus.call("doc:step", expr, 1, 1)
            if focus.call("doc:step", expr, 1, 0) == 0x10000a:
                extra = "\t"
            else:
                extra = focus.call("doc:get-str", indent_end, expr, ret='str')
                extra = "".ljust(len(extra),' ')
        else:
                extra = ""

        return focus.call("Replace", 1, m, "\n" + new + extra)

    def find_indent(self, focus, m):
        # Find previous line which is not empty and return
        # a string containing the leading tabs/spaces.
        # The mark is moved to the end of the indent.
        while self.call("doc:step", focus, 0, m) == 0x10000a:
            self.call("doc:step", focus, 0, 1, m)
        if self.call("doc:step", focus, 0, m) == edlib.WEOF:
            return ""
        # line before m is not empty
        m2 = m.dup()
        c = self.call("doc:step", focus, 0, m2)
        while c != edlib.WEOF and (c & 0xfffff) != 10:
            self.call("doc:step", focus, 0, 1, m2)
            if chr(c&0xfffff) not in " \t":
                m.to_mark(m2)
            c = self.call("doc:step", focus, 0, m2)
        # m2 .. m is the prefix
        return focus.call("doc:get-str", m2, m, ret = 'str')

def c_mode_attach(key, focus, comm2, **a):
    p = focus.render_attach("text")
    p = CModePane(p)
    comm2("callback", p)
    return 1

def py_mode_attach(key, focus, comm2, **a):
    p = focus.render_attach("text")
    p = CModePane(p)
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
