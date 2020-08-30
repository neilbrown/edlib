# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

# This pane highlights merge results.
# This is sections of text between <<<< |||| ==== >>>> marker lines.
# There are 3 sections and any two can be compared.
# With the cursor on <<<< or ||||| the next two sections are compared.
# With the cursor on ==== the first and last are compared.
# With the cursor on >>>>, we move to the next merge.
# If cursor is not on any, it is moved forward to the next one.
#

def measure(p, start, end):
    start = start.dup()
    len = 0
    while start < end:
        if p.call("Move-Char", start, 1) <= 0:
            break
        len += 1
    return len

class MergePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.marks = None
        self.difftype = 0
        self.call("doc:request:doc:replaced")

    def fore(self, m, ptn):
        if not m:
            return None
        m = m.dup()
        if self.call("text-search", m, "^(?4:"+ptn+")") > 1:
            self.call("Move-EOL", -1, m)
            return m
        return None

    def back(self, m, ptn):
        if not m:
            return None
        m = m.dup()
        if self.call("text-search", 0, 1,  m, "^(?4:"+ptn+")") > 1:
            self.call("Move-EOL", -1, m)
            return m
        return None

    def mark(self, m1s, m1e, m2s, m2e, move=True):
        if not m1s or not m1e or not m2s or not m2e:
            self.call("Message:modal", "failed to find merge markers")
            return 1
        if move:
            self.call("Move-EOL", 1, m1s); self.call("Move-Char", 1, m1s)
            self.call("Move-EOL", 1, m2s); self.call("Move-Char", 1, m2s)
        else:
            self.call("doc:set-attr", "render:merge-same", m1s, m1e)
            self.call("doc:set-attr", "render:merge-same", m2s, m2e)

        a = measure(self, m1s, m1e)
        b = measure(self, m2s, m2e)

        if a > 0 and b > 0:
            ret = self.call("WordDiff", m1s, a, m2s, b, "render:merge-same")
        else:
            # everything different
            ret = 4
        self.marks = [m1s,m1e, m2s,m2e]
        self.difftype = ret
        self.call("view:changed", m1s, m1e)
        self.call("view:changed", m2s, m2e)
        return 1

    def handle_alt_m(self, key, focus, mark, **a):
        "handle:K:M-m"

        if self.marks:
            focus.call("doc:set-attr", "render:merge-same",
                       self.marks[0], self.marks[1])
            focus.call("doc:set-attr", "render:merge-same",
                       self.marks[2], self.marks[3])
            self.marks = None

        if not mark:
            return
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        try:
            focus.call("text-search", m, "^(<<<<|\\|{4}|====)")
            focus.call("Move-EOL", -1, m)
        except edlib.commandfailed:
            pass
        if focus.call("text-match", m.dup(), "<<<<") > 1:
            m1 = self.fore(m, "||||")
            m2 = self.fore(m1, "====")
            self.mark(m,m1,m1.dup(),m2)
            mark.to_mark(m1)
        elif focus.call("text-match", m.dup(), "?4:||||") > 1:
            m1 = self.fore(m, "====")
            m2 = self.fore(m, ">>>>")
            self.mark(m, m1, m1.dup(), m2)
            mark.to_mark(m1)
        elif focus.call("text-match", m.dup(), "====") > 1:
            m1 = self.back(m, "||||")
            m0 = self.back(m1, "<<<<")
            m2 = self.fore(m, ">>>>")
            self.mark(m0, m1, m, m2)
            mark.to_mark(m2)
        else:
            self.call("Message:modal", "Cannot find a merge mark")
        return 1

    def handle_update(self, key, focus, mark, mark2, num, num2, **a):
        "handle:doc:replaced"
        if num2:
            # only attrs updated
            return 0
        if not self.marks:
            return 0
        # only update if an endpoint is in the range.
        if ((mark and mark >= self.marks[0] and mark <= self.marks[1]) or
            (mark and mark >= self.marks[2] and mark <= self.marks[3]) or
            (mark2 and mark2 >= self.marks[0] and mark2 <= self.marks[1]) or
            (mark2 and mark2 >= self.marks[2] and mark2 <= self.marks[3])):
            # Update the highlight
            self.mark(*self.marks, move=False)
            return 0

    def handle_highlight(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2 or not mark:
            return

        if str == "start-of-line":
            if not self.marks:
                return
            s1,e1,s2,e2 = self.marks
            if self.difftype == 1:
                # No difference, no 'merge-same' attrs,
                if mark >= s1 and mark < e1:
                    comm2("attr:cb", focus, mark, "fg:red-40,nobold",
                          10000, 2)
                if mark >= s2 and mark < e2:
                    comm2("attr:cb", focus, mark, "fg:green-40,nobold",
                          10000, 2)
            else:
                if mark >= s1 and mark < e1:
                    comm2("attr:cb", focus, mark, "fg:red-60,bg:magenta+90,bold",
                          10000, 2)
                if mark >= s2 and mark < e2:
                    comm2("attr:cb", focus, mark, "fg:green-60,bg:cyan+90,bold",
                          10000, 2)
            return
        if str == "render:merge-same":
            w = str2.split()
            len = int(w[0])
            if w[1] == '1':
                # This is the '+' section
                comm2("attr:cb", focus, mark, "fg:green-40,bg:white,nobold",
                      len, 3)
            else:
                comm2("attr:cb", focus, mark, "fg:red-40,bg:white,nobold",
                      len, 3)
            return 0


def merge_view_attach(key, focus, comm2, **a):
    p = MergePane(focus)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

def add_merge(key, focus, **a):
    p = MergePane(focus)
    if p:
        p.call("view:changed")

    v = focus['view-default']
    if v:
        v = v + ',merge'
    else:
        v = 'merge'
    focus.call("doc:set:view-default", v)
    return 1

editor.call("global-set-command", "attach-merge", merge_view_attach)
editor.call("global-set-command", "interactive-cmd-merge-mode", add_merge)
editor.call("global-load-module", "lib-worddiff")
