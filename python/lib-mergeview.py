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

class MergePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.marks = None
        self.conflicts = 0
        self.call("doc:request:doc:replaced")

    def fore(self, m, end, ptn):
        if not m:
            return None
        m = m.dup()
        try:
            if self.call("text-search", m, end, "^(?4:"+ptn+")") > 1:
                self.call("Move-EOL", -1, m)
                return m
        except edlib.commandfailed:
            pass
        return None

    def add_pair(self, v, group, m1, m2):
        vm = edlib.Mark(self, v)
        vm.to_mark(m1);
        self.call("Move-EOL", 1, vm);
        self.call("Move-Char", 1, vm)
        vm['wiggle'] = group + "-start"
        vm1 = edlib.Mark(orig=vm)
        vm1.to_mark(m2)
        vm1['wiggle'] = group + "-end"
        return vm

    def mark(self, start, end):
        # There is probably a 3-way merge between start and end

        if self.marks:
            self.call("doc:set-attr", "render:merge-same",
                      self.marks[0], self.marks[3])
            self.marks = None
        start = self.fore(start, end, "<<<<")
        m1 = self.fore(start, end, "||||")
        m2 = self.fore(m1, end, "====")
        m3 = self.fore(m2, end, ">>>>")
        if not m3:
            # something wasn't found, give up
            return
        v = self.call("doc:add-view") - 1
        vm = self.add_pair(v, "orig", start, m1)
        self.add_pair(v, "before", m1, m2)
        self.add_pair(v, "after", m2, m3)

        ret = self.call("WordWiggle", "render:merge-same", vm)

        m = self.call("doc:vmark-get", v, ret='mark')
        while m:
            m.release()
            m = self.call("doc:vmark-get", v, ret='mark')
        self.call("doc:del-view", v)

        self.marks = [start, m1, m2, m3]
        self.conflicts = ret - 1
        self.call("view:changed", start, m3)
        return 1

    def handle_alt_m(self, key, focus, mark, **a):
        "handle:K:M-m"

        if self.marks:
            focus.call("doc:set-attr", "render:merge-same",
                       self.marks[0], self.marks[3])
            self.marks = None

        if not mark:
            return
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        try:
            focus.call("text-search", m, "^(<<<<|>>>>)")
            focus.call("Move-EOL", -1, m)
        except edlib.commandfailed:
            self.call("Message:modal", "Cannot find a merge mark")
            return edlib.Efalse
        if focus.call("text-match", m.dup(), ">>>>") > 1:
            # was inside a merge, move to start
            try:
                # search backwards
                focus.call("text-search", m, 0,1, "^<<<<")
            except edlib.commandfailed:
                # weird, no start,  I guess we give up
                self.call("Message:modal", "Cannot find a merge mark")
                return edlib.Efalse
            focus.call("Move-EOL", -1, m)
        # must be at the start.
        try:
            end = m.dup()
            focus.call("Move-EOL", 1, end)
            focus.call("text-search", end, "^(<<<<|>>>>)")
            focus.call("Move-EOL", -1, end)
        except edlib.commandfailed:
            # There is no end
            return edlib.Efalse
        if focus.call("doc:step", 1, end, ret='char') != '>':
            # didn't find a matching end.
            mark.to_mark(end)
            self.call("Message:modal", "Merge wasn't terminated, next is here")
            return edlib.Efalse

        m1 = self.fore(m, end, "||||")
        m2 = self.fore(m1, end, "====")
        m3 = end
        if m3:
            self.call("Move-EOL", 1, m3)
            self.call("Move-Char", 1, m3)
            self.mark(m, m3)
            mark.to_mark(m3)
        return 1

    def remark(self, key, **a):
        if self.marks:
            m = self.marks[3].dup()
            self.call("Move-EOL", 1, m)
            self.call("Move-Char", 1, m)
            self.mark(self.marks[0], m)
        return edlib.Efalse


    def handle_update(self, key, focus, mark, mark2, num, num2, **a):
        "handle:doc:replaced"
        if num2:
            # only attrs updated
            return 0
        if not self.marks:
            return 0
        # only update if an endpoint is in the range.
        if ((mark and mark >= self.marks[0] and mark <= self.marks[3]) or
            (mark2 and mark2 >= self.marks[0] and mark2 <= self.marks[3])):
            # Update the highlight
            self.call("event:timer", 10, self.remark)
            return 0

    def handle_highlight(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2 or not mark:
            return

        if not self.marks:
            return
        o,b,a,e = self.marks

        if str == "start-of-line":
            if mark == o or mark == b or mark == a or mark == e:
                if self.conflicts:
                    comm2("attr:cb", focus, mark, "fg:red-40",
                          10000, 2)
                else:
                    comm2("attr:cb", focus, mark, "fg:green-40",
                          10000, 2)
            return

        if str == "render:merge-same":
            w = str2.split()
            len = int(w[0])
            if w[1] == "Unmatched":
                comm2("attr:cb", focus, mark, "fg:blue-40", len, 3)
            if w[1] == "Extraneous":
                comm2("attr:cb", focus, mark, "fg:cyan-40", len, 3)
            if w[1] == "Changed":
                if mark < a:
                    comm2("attr:cb", focus, mark, "fg:red-40", len, 3)
                else:
                    comm2("attr:cb", focus, mark, "fg:green-40", len, 3)
            if w[1] == "Conflict":
                comm2("attr:cb", focus, mark, "fg:red-40,inverse", len, 3)
            if w[1] == "AlreadyApplied":
                comm2("attr:cb", focus, mark, "fg:cyan-40,inverse", len, 3)

            return 0


def merge_view_attach(key, focus, comm2, **a):
    p = MergePane(focus)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

def add_merge(key, focus, mark, **a):
    p = MergePane(focus)
    if p:
        p.call("view:changed")

    v = focus['view-default']
    if v:
        v = v + ',merge'
    else:
        v = 'merge'
    focus.call("doc:set:view-default", v)
    if mark:
        p.call("K:M-m", focus, mark)
    return 1

editor.call("global-set-command", "attach-merge", merge_view_attach)
editor.call("global-set-command", "interactive-cmd-merge-mode", add_merge)
editor.call("global-load-module", "lib-worddiff")
