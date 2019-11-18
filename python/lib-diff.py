# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

# This pane overlays a pane showing unified diff output and:
# - colourizes + and - lines
# - interprets 'Enter' to find the given line

import os.path


def djoin(dir, tail):
    # 'tail' might not exist at 'dir', but might exist below some
    # prefix for dir.  We want to find that prefix and add it.
    orig = dir
    while dir and not os.path.exists(os.path.join(dir,tail)):
        dir = os.path.dirname(dir)
    if not dir:
        dir = orig
    return os.path.join(dir, tail)

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


    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        lines = 0
        while focus.call("doc:step", 0, m, ret='char') == '\n':
            try:
                focus.call("text-match", m,
                           "@@ -[\d]+,[\d]+ \+[\d]+,[\d]+ @@")
                break
            except edlib.commandfailed:
                pass
            c = focus.call("doc:step", m, 1, ret='char')
            if c in '+ ':
                lines += 1
            focus.call("Move-EOL", -2, m)
        focus.call("Move-EOL", -1, m)
        focus.call("text-match", m, "@@ -[\d]+,[\d]+ \+")
        ms = m.dup()
        focus.call("text-match", m, "[\d]+")
        s = focus.call("doc:get-str", ms, m, ret='str')
        # need to find a line starting '+++' immediately before one starting '@@'
        at_at = True
        found_plus = False
        while not found_plus:
            focus.call("Move-EOL", -2, m)
            try:
                if at_at:
                    focus.call("text-match", m, "\+\+\+ ")
                    found_plus = True
                else:
                    focus.call("text-match", m, "@@ ")
                    at_at = True
            except edlib.commandfailed:
                at_at = False
        if not found_plus:
            return edlib.Efail
        ms = m.dup()
        focus.call("Move-EOL", 1, m)
        fname = focus.call("doc:get-str", ms, m, ret='str')
        if fname.startswith('b/'):
            fname = fname[2:]
        if fname[0] != '/':
            fname = djoin(focus['dirname'], fname)
        lines = int(s) + lines - 1
        try:
            d = focus.call("doc:open", -1, 8, fname, ret='focus')
        except edlib.commandfailed:
            d = None
        if not d:
            focus.call("Message", "File %s not found" % fname)
            return edlib.Efail

        par = focus.call("DocPane", d, ret='focus')
        if par:
            while par.focus:
                par = par.focus
        else:
            par = focus.call("OtherPane", d, ret='focus')
            if not par:
                focus.call("Message", "Failed to open pane")
                return edlib.Efail
            par = d.call("doc:attach-view", par, 1, ret='focus')
        par.take_focus()
        par.call("Move-File", -1)
        par.call("Move-Line",lines - 1)

        return 1

def diff_view_attach(key, focus, comm2, **a):
    p = focus.call("attach-viewer", ret='focus')
    p = DiffPane(p)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

editor.call("global-set-command", "attach-diff", diff_view_attach)
