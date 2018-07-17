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
        return self.call("Replace", focus, 1, m, "\n")

def c_mode_attach(key, focus, comm2, **a):
    p = focus.render_attach("text")
    p = CModePane(p)
    comm2("callback", p)
    return 1

def c_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-2:] in [".c", ".h"]:
        focus["render-default"] = "c-mode"
    return 0

editor.call("global-set-command", "doc:appeared-c-mode", c_mode_appeared)
editor.call("global-set-command", "attach-render-c-mode", c_mode_attach)