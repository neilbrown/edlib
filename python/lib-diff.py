# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

# This pane overlays a pane showing unified diff output and:
# - colourizes + and - lines
# - interprets 'Enter' to find the given line

class DiffPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_highlight(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2:
            return
        if str == "start-of-line":
            c = focus.call("doc:step", 1, mark, ret='char')
            if c == '+':
                comm2("attr:cb", focus, mark, "fg:green-40", 1000000, 1)
            if c == '-':
                comm2("attr:cb", focus, mark, "fg:red-40", 1000000, 1)
            return 0

def diff_view_attach(key, focus, comm2, **a):
    p = focus.call("attach-viewer", ret='focus')
    p = DiffPane(p)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

editor.call("global-set-command", "attach-diff", diff_view_attach)
