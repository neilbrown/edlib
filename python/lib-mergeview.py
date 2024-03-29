# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2020-2023 <neil@brown.name>
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

import edlib

class MergePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.marks = None
        self.done_marks = None
        self.wig = None
        self.conflicts = 0
        self.space_conflicts = 0
        self.merge_num = None
        self.merge_char = None
        self.call("doc:request:doc:replaced")
        self.call("doc:request:mark:moving")
        if focus["mergeview-autofind"] == "true":
            focus.call("doc:set:mergeview-autofind")
            m = self.call("doc:point", ret='mark')
            self.call("K:A-m", self, m)

    def fore(self, m, end, ptn):
        if not m:
            return None
        m = m.dup()
        try:
            if self.call("text-search", m, end, "^(?4:"+ptn+")") > 1:
                self.call("doc:EOL", -1, m)
                return m
        except edlib.commandfailed:
            pass
        return None

    def mark(self, start, end):
        # There is probably a 3-way merge between start and end

        if self.marks:
            self.call("doc:set-attr", "render:merge-same",
                      self.marks[0], self.marks[3])
            self.marks = None
        self.wig = None
        start = self.fore(start, end, "<<<<")
        m1 = self.fore(start, end, "||||")
        m2 = self.fore(m1, end, "====")
        m3 = self.fore(m2, end, ">>>>")
        if not m3:
            # something wasn't found, give up
            return

        cmd = self.call("MakeWiggle", ret='comm')
        t = start.dup()
        self.call("doc:EOL", 1, t, 1)
        cmd("orig", self, t, m1)

        t = m1.dup()
        self.call("doc:EOL", 1, t, 1)
        cmd("before", self, t, m2)

        t = m2.dup()
        self.call("doc:EOL", 1, t, 1)
        cmd("after", self, t, m3)

        ret = cmd("set-wiggle", self, "render:merge-same")
        self.conflicts = ret-1
        self.space_conflicts = cmd("get-result", self, "space-conflicts") - 1
        if self.conflicts == self.space_conflicts:
            self.wig = cmd("get-result", self, "wiggle", ret='str')

        del cmd

        self.marks = [start, m1, m2, m3]
        self.call("view:changed", start, m3)
        return 1

    def done(self, focus, start, end):
        self.done_marks = [ start.dup(), end.dup() ]
        self.marks = None
        focus.call("view:changed", self.done_marks[0], self.done_marks[1])

    def apply_copied_merge(self, focus):
        # If we have a selection and last copy was a patch, insert it
        pt = focus.call("doc:point", ret='mark')
        if pt and pt['selection:active'] == "1":
            mk = focus.call("doc:point", ret='mark2')
        else:
            mk = None
        if mk:
            diff = focus.call("copy:get", ret='str')
            if not diff or not diff.startswith("|||||||"):
                diff = None
        else:
            diff = None
        if diff:
            # selection must be full lines
            focus.call("doc:EOL", -1, pt)
            focus.call("doc:EOL", -1, mk)
            strt,end = pt.dup(),mk.dup()
            if strt > end:
                strt,end = end,strt
            focus.call("select:commit")

            # move strt before region
            strt.step(0)
            focus.call("doc:replace", strt, strt, "<<<<<<< found\n")
            # move end after region
            end.step(1)
            focus.call("doc:replace", end, end, diff, 0, 1)
            return strt
        return None

    def handle_alt_m(self, key, focus, num, mark, **a):
        "handle:K:A-m"

        if num != edlib.NO_NUMERIC and self.marks:
            # we have a numeric arg - act on current merge
            if num == -edlib.NO_NUMERIC:
                focus.call("doc:set-attr", "render:merge-same",
                           self.marks[0], self.marks[3])
                # Simple -ve prefix - keep original.
                m = self.marks[0].dup()
                focus.call("doc:EOL", 1, 1, m)
                # Remove first marker
                focus.call("doc:replace", self.marks[0], m)
                m = self.marks[3].dup()
                focus.call("doc:EOL", 1, 1, m)
                # Remove before/after section with markers
                focus.call("doc:replace", 0, 1, self.marks[1], m)
                self.done(focus, self.marks[0], self.marks[1])
                return 1
            if num == 0:
                # if no conflicts remain, wiggle the merge
                if self.wig is None:
                    focus.call("Message", "Cannot complete merge while conflicts remain")
                    return 1
                focus.call("doc:set-attr", "render:merge-same",
                           self.marks[0], self.marks[3])

                m = self.marks[3].dup()
                focus.call("doc:EOL", 1, 1, m)
                focus.call("doc:replace", self.marks[0], m, self.wig)
                self.done(focus, self.marks[0], m)
                return 1
            if num == 1:
                focus.call("doc:set-attr", "render:merge-same",
                           self.marks[0], self.marks[3])
                # Ignore conflucts, keep replacement
                m = self.marks[2].dup()
                focus.call("doc:EOL", 1, 1, m)
                # Remove first orig/before and markers
                focus.call("doc:replace", self.marks[0], m)
                m = self.marks[3].dup()
                focus.call("doc:EOL", 1, 1, m)
                # Remove final marker
                focus.call("doc:replace", 0, 1, self.marks[3], m)
                self.done(focus, self.marks[2], self.marks[3])
                return 1
            if num == 9:
                focus.call("doc:set-attr", "render:merge-same",
                           self.marks[0], self.marks[3])
                # Cut the before/after into the copy buffer.
                m = self.marks[0].dup()
                focus.call("doc:EOL", 1, 1, m)
                # Remove first marker
                focus.call("doc:replace", self.marks[0], m)
                m = self.marks[3].dup()
                focus.call("doc:EOL", 1, 1, m)
                # Now cut Remove before/after section with markers
                diff = focus.call("doc:get-str", self.marks[1], m, ret='str')
                focus.call("copy:save", diff)
                focus.call("doc:replace", 0, 1, self.marks[1], m)
                self.marks = None
                # we have an active selection, insert the merge there.
                mark = self.apply_copied_merge(focus)
                if not mark:
                    return 1
                num = 1
            else:
                return 1

        if num == 9:
            # paste from copy-buf if it is a diff
            m = self.apply_copied_merge(focus)
            if not m:
                return 1
            mark = m
            # fall through to "Find" the merge

        if self.marks:
            focus.call("doc:set-attr", "render:merge-same",
                       self.marks[0], self.marks[3])
            self.marks = None
        if self.done_marks:
            focus.call("view:changed", self.done_marks[0], self.done_marks[1])
            self.done_marks = None

        if not mark:
            return
        m = mark.dup()
        focus.call("doc:EOL", -1, m)
        try:
            focus.call("text-search", m, "^(<<<<|>>>>)")
            focus.call("doc:EOL", -1, m)
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
            focus.call("doc:EOL", -1, m)
        # must be at the start.
        try:
            end = m.dup()
            focus.call("doc:EOL", 1, end)
            focus.call("text-search", end, "^(<<<<|>>>>)")
            focus.call("doc:EOL", -1, end)
        except edlib.commandfailed:
            # There is no end
            return edlib.Efalse
        if focus.following(end) != '>':
            # didn't find a matching end.
            mark.to_mark(end)
            self.call("Message:modal", "Merge wasn't terminated, next is here")
            return edlib.Efalse

        m1 = self.fore(m, end, "||||")
        m2 = self.fore(m1, end, "====")
        m3 = end
        if m3:
            self.call("doc:EOL", 1, m3, 1)
            self.mark(m, m3)
            mark.to_mark(m3)
        return 1

    def handle_shift(self, key, focus, num, mark, **a):
        "handle-list/K-</K->"
        if not self.marks:
            return edlib.Efallthrough
        m = mark.dup()
        focus.call("doc:EOL", m, -1)
        if m == self.marks[0]:
            if key[-1] == '<':
                # Move one line from before the header to after.
                m2 = m.dup()
                focus.call("doc:EOL", m2, -2)
                if m2 == m:
                    # Nothing to move
                    return 1
                txt = focus.call("doc:get-str", m2, m, ret='str')
                if txt.startswith(">>>>>>>"):
                    # We've run into another chunk - stop
                    return 1
                m3 = mark.dup()
                focus.call("doc:EOL", m3, 1, 1)
                focus.call("doc:replace", m3, m3, txt)
                focus.call("doc:replace", m2, m, 0, 1)
                return 1
            else:
                # Move one line from after the header to before
                m2 = mark.dup()
                focus.call("doc:EOL", m2, 1, 1)
                m3 = m2.dup()
                focus.call("doc:EOL", m3, 1, 1)
                if m3 == m2:
                    return 1
                txt = focus.call("doc:get-str", m2, m3, ret='str')
                if txt.startswith("|||||||"):
                    # Nothing here
                    return 1
                m.step(0)
                focus.call("doc:replace", m, m, txt)
                focus.call("doc:replace", m2, m3, 0, 1)
                return 1
            return 1;
        if m == self.marks[1]:
            if key[-1] == '<':
                # Move one line from before marker to after end
                m2 = m.dup()
                focus.call("doc:EOL", m2, -2)
                if m2 == m:
                    # Nothing to move
                    return 1
                txt = focus.call("doc:get-str", m2, m, ret='str')
                if txt.startswith("<<<<<<<"):
                    # We've run out of orig
                    return 1
                m3 = self.marks[3].dup()
                focus.call("doc:EOL", m3, 1, 1)
                focus.call("doc:replace", m3, m3, txt)
                focus.call("doc:replace", m2, m, 0, 1)
                return 1
            else:
                # Move one line from after end marker to before
                m2 = self.marks[3].dup()
                focus.call("doc:EOL", m2, 1, 1)
                m3 = m2.dup()
                focus.call("doc:EOL", m3, 1, 1)
                if m3 == m2:
                    return 1
                txt = focus.call("doc:get-str", m2, m3, ret='str')
                if txt.startswith("<<<<<<<"):
                    # Run into next chunk
                    return 1
                m.step(0)
                focus.call("doc:replace", m, m, txt)
                focus.call("doc:replace", m2, m3, 0, 1)
                return 1
            return 1;
        if m == self.marks[2]:
            # I don't know what, if anything, I want here.
            return 1;
        if m == self.marks[3]:
            return 1;
        return edlib.Efallthrough

    def handle_swap(self, key, focus, mark, **a):
        "handle:mode-swap-mark"
        # redefine mode-swap-mark while in merge to cycle among the branches.
        if not self.marks:
            return edlib.Efallthrough
        m = mark.dup()
        focus.call("doc:EOL", m, -1)
        if m <= self.marks[0]:
            mark.to_mark(self.marks[1])
            return 1
        if m == self.marks[1]:
            mark.to_mark(self.marks[2])
            return 1
        if m == self.marks[2]:
            mark.to_mark(self.marks[3])
            return 1
        if m >= self.marks[3]:
            mark.to_mark(self.marks[0])
            return 1

        # Must be in the merge - find the location and cycle
        if m < self.marks[1]:
            which = 0
        elif m < self.marks[2]:
            which = 1
        else:
            which = 2
        if self.merge_num is None:
            m.to_mark(mark)
            a = focus.call("doc:get-attr", m, "render:merge-same", ret='str')
            c = 0
            while (not a or a[0] != 'M') and m > self.marks[which]:
                focus.prev(m)
                c += 1
                a = focus.call("doc:get-attr", m, "render:merge-same", ret='str')
            if not a:
                return 1
            self.merge_num = int(a.split()[3])
            self.merge_char = c
        which = (which + 1) % 3
        m.to_mark(self.marks[which])
        focus.call("doc:EOL", m, 1, 1)
        a = focus.call("doc:get-attr", m, "render:merge-same", ret='str')
        while (m < self.marks[which+1] and
               (not a or a[0] != 'M' or int(a.split()[3]) < self.merge_num)):
            focus.next(m)
            a = focus.call("doc:get-attr", m, "render:merge-same", ret='str')
        if not a:
            return 1
        if int(a.split()[3]) == self.merge_num:
            # target merge exists - find the char
            c = 0
            a = None
            while (m < self.marks[which+1] and
                   c < self.merge_char and
                   (not a or a[0] != 'M')):
                focus.next(m)
                c += 1
                a = focus.call("doc:get-attr", m, "render:merge-same", ret='str')
        # when we move, merge_num will be cleared!
        mn = self.merge_num
        focus.call("Move-to", m)
        self.merge_num = mn
        return 1

    def handle_mark_moving(self, key, focus, mark, **a):
        "handle:mark:moving"
        pt = self.call("doc:point", ret='mark')
        if pt == mark:
            self.merge_num = None
        return 1

    def remark(self, key, **a):
        if self.marks:
            m = self.marks[3].dup()
            self.call("doc:EOL", 1, m, 1)
            self.mark(self.marks[0], m)
        return edlib.Efalse

    def handle_update(self, key, focus, mark, mark2, num, num2, **a):
        "handle:doc:replaced"
        if not self.marks:
            return 0
        # only update if an endpoint is in the range.
        if ((mark and mark >= self.marks[0] and mark <= self.marks[3]) or
            (mark2 and mark2 >= self.marks[0] and mark2 <= self.marks[3])):
            # Update the highlight
            self.call("event:on-idle", self.remark)
            return 0

    def handle_highlight(self, key, focus, str1, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2 or not mark:
            return

        if self.done_marks and str1 == "start-of-line":
            if mark >= self.done_marks[0] and mark < self.done_marks[1]:
                comm2("attr:cb", focus, mark, "fg:green-60,inverse", 10000, 220)
                return

        if not self.marks:
            return
        o,b,a,e = self.marks

        if str1 == "start-of-line":
            if mark == o or mark == b or mark == a or mark == e:
                if self.conflicts > self.space_conflicts:
                    comm2("attr:cb", focus, mark, "fg:red-40",
                          0, 102)
                elif self.conflicts:
                    comm2("attr:cb", focus, mark, "fg:blue-80",
                          0, 102)
                else:
                    comm2("attr:cb", focus, mark, "fg:green-60,bold",
                          0, 102)
            return edlib.Efallthrough

        if str1 == "render:merge-same":
            # [ML] len type num {spaces}
            w = str2.split()
            alen = int(w[1])
            type = w[2]
            spaces = w[4] if len(w) >=5 else ""
            if type == "Unmatched":
                comm2("attr:cb", focus, mark, "fg:blue-80,bg:cyan+20", alen, 103)
            if type == "Extraneous":
                comm2("attr:cb", focus, mark, "fg:cyan-60,bg:yellow", alen, 103)
            if type == "Changed":
                if mark < a:
                    comm2("attr:cb", focus, mark, "fg:red-60", alen, 103)
                else:
                    comm2("attr:cb", focus, mark, "fg:green-60", alen, 103)
            if type == "Conflict":
                if spaces == "spaces":
                    comm2("attr:cb", focus, mark, "fg:red-60,underline", alen, 103)
                else:
                    comm2("attr:cb", focus, mark, "fg:red-60,inverse", alen, 103)
            if type == "AlreadyApplied":
                if mark > b and mark < a:
                    # This part is 'before' - mosly irrelevant
                    comm2("attr:cb", focus, mark, "fg:cyan-60", alen, 103)
                else:
                    comm2("attr:cb", focus, mark, "fg:cyan-60,inverse", alen, 103)

            return edlib.Efallthrough

def merge_appeared(key, focus, **a):
    t = focus["doc-type"]
    if t != "text":
        return 1
    m = edlib.Mark(focus)
    try:
        focus.call("text-search", m, "^<<<<<<<")
        focus.call("doc:append:view-default", ",merge")
        focus.call("doc:set:mergeview-autofind", "true")
    except:
        pass

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

    focus.call("doc:append:view-default", ",merge")
    if mark:
        p.call("K:A-m", focus, mark)
    return 1

edlib.editor.call("global-set-command", "attach-merge", merge_view_attach)
edlib.editor.call("global-set-command", "interactive-cmd-merge-mode", add_merge)
edlib.editor.call("global-set-command", "doc:appeared-mergeview", merge_appeared)
