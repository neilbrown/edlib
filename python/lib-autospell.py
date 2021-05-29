# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# autospell: spell-check visible words in a document and highlight
#    those that aren't in a dictionary
#

# We keep a list of marks which alternate between the start and end
# of a "checked" section of text.  The 'start' marks have an attribute
# ('spell:start') set.  The 'end' marks do not.
# We remove ranges from this list when the document is changed.  This
# might require splitting a 'checked' section, or removing some completely.
# We add ranges when a word is checked.  This might merge with existing
# ranges.
# We need to find an unchecked section in some range to start checking.
# This is all managed by:
#   remove_range(focus, viewnum, attr, start, end)
#   add_range(focus, viewnum, attr, start, end)
#   choose_range(focus, viewnum, attr, start, end) - changes start and end to be
#    a contiguous unchecked section in the range

def show_range(action, focus, viewnum, attr):
    edlib.LOG("range:", attr, action)
    f,l = focus.vmarks(viewnum)
    while f:
        edlib.LOG("  ", f, f[attr])
        f = f.next()
    edlib.LOG("done", action)

def remove_range(focus, viewnum, attr, start, end):
    m = focus.vmark_at_or_before(viewnum, start)
    if m and not m[attr]:
        # immediately after start is not active, so the earlist we might need
        # to remove is the next mark, or possibly the very first
        if m:
            m = m.next()
        else:
            m, l = focus.vmarks(viewnum)
        if not m or m > end:
            # Nothing to remove
            return
    else:
        # from m to start are in a range and should stay there.
        # split the range from 'm' at 'start'
        m = edlib.Mark(focus, view = viewnum)
        m.to_mark(start)
        m = edlib.Mark(orig=m, owner = focus)
        # ensure the m is after the previous one
        m.step(1)
        m[attr] = 'yes'
    # m is now the start of an active section that is within start-end
    # and should be removed
    m2 = focus.vmark_at_or_before(viewnum, end)
    if m2 and m2 == end and m2[attr]:
        # this section is entirely after end, so not interesting
        m2 = m2.prev()
    if m2 and m2[attr]:
        # end is within an active section that needs to be split
        m2 = edlib.Mark(focus, view = viewnum)
        m2.to_mark(end)
        m2[attr] = 'yes'
        m2 = edlib.Mark(orig=m2, owner=focus)
        m2.step(0)
    # m2 is now the end of an active section tht needs to be
    # discarded
    while m < m2:
        old = m
        m = m.next()
        old.release()
    m2.release()
    return

def add_range(focus, viewnum, attr, start, end):
    m1 = focus.vmark_at_or_before(viewnum, start)
    if m1 and m1[attr]:
        m1 = m1.next()
        # can move m1 down as needed
    elif m1 and m1 == start:
        # can move m1 down
        pass
    else:
        m1 = None
        # must create new mark, or move a later mark up
    m2 = focus.vmark_at_or_before(viewnum, end)
    if m2 and not m2[attr]:
        if m2 == end:
             m2 = m2.prev()
             # can move m2 earlier
        else:
            # end not in range, must create mark or move earlier up
            m2 = None
    # if m2, then can move it backwards.  No need to create
    if not m1 and not m2:
        # no overlaps, create new region
        m1 = edlib.Mark(focus, viewnum)
        m1.to_mark(start)
        m2 = edlib.Mark(orig=m1, owner=focus)
        m2.to_mark(end)
        m1[attr] = 'yes'
    elif m1 and not m2:
        # can move m1 down to end, removing anything in the way
        m = m1.next()
        while m and m <= end:
            m.release()
            m = m1.next()
        m1.to_mark(end)
    elif not m1 and m2:
        # can move m2 up to start, removing things
        m = m2.prev()
        while m and m >= start:
            m.release()
            m = m2.prev()
        m2.to_mark(start)
    else:
        # can remove all from m1 to m2 inclusive
        while m1 < m2:
            m = m1.next()
            m1.release()
            m1 = m
        m2.release()

def choose_range(focus, viewnum, attr, start, end):
    # contract start-end so that none of it is in-range
    m1 = focus.vmark_at_or_before(viewnum, start)
    if m1 and not m1[attr]:
        m2 = m1.next()
        # start not in-range, end must not exceed m1
    elif m1 and m1[attr]:
        # start is in range - move it forward
        m1 = m1.next()
        if m1:
            start.to_mark(m1)
            m2 = m1.next()
        else:
            # error
            m2 = start
    else:
        m2, l = focus.vmarks(viewnum)
    if m2 and m2 < end:
        end.to_mark(m2)

class autospell_monitor(edlib.Pane):
    # autospell_monitor attaches to a document and track ranges
    # that have been spell-checked.  Sends notifications when there
    # is a change, so viewers can do the checking.  Only views know
    # mode-specific details
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.view = self.call("doc:add-view") - 1
        self.call("doc:request:doc:replaced")
        self.call("doc:request:spell:mark-checked")
        self.call("doc:request:spell:choose-range")

    def doc_replace(self, key, focus, mark, mark2, num2, **a):
        "handle:doc:replaced"
        if num2:
            # only attrs changed
            return 1
        if not mark or not mark2:
            # Should I clean up completely?
            return 1
        # mark2 might have been the start-of-word, but not any longer
        # So any spell-incorrect must be cleared as normal checking
        # only affects first char of a word.
        focus.call("doc:set-attr", mark2, "render:spell-incorrect",
                   None);
        # Need to caputure adjacent words, and avoid zero-size gap
        mark = mark.dup()
        focus.prev(mark)
        mark2 = mark2.dup()
        focus.next(mark2)

        remove_range(self, self.view, "spell:start", mark, mark2)

        self.call("doc:notify:spell:recheck")
        return 1

    def handle_checked(self, key, mark, mark2, **a):
        "handle:spell:mark-checked"
        if mark and mark2:
            add_range(self, self.view, 'spell:start', mark, mark2)
        return 1

    def handle_choose(self, key, mark, mark2, **a):
        "handle:spell:choose-range"
        if mark and mark2:
            choose_range(self, self.view, 'spell:start', mark, mark2)
        return 1

class autospell_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.scheduled = False
        self.helper_attached = False
        # visible region
        self.vstart = None
        self.vend = None
        self.call("doc:request:spell:recheck")
        # trigger render-lines refresh notification
        pt = focus.call("doc:point", ret='mark')
        focus.call("render:request:reposition", pt)

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = autospell_view(focus)
        self.clone_children(p)
        return 1

    def handle_clip(self, key, mark, mark2, num, **a):
        "handle:Notify:clip"
        if self.vstart:
            self.vstart.clip(mark, mark2)
        if self.vend:
            self.vend.clip(mark, mark2)
        return edlib.Efallthrough

    def map_attr(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not str or not mark or not comm2:
            return edlib.Enoarg
        if str == "render:spell-incorrect":
            comm2("cb", focus, int(str2), mark, "fg:red-80,underline", 100)
        return edlib.Efallthrough

    def handle_recheck(self, key, **a):
        "handle:spell:recheck"
        self.sched()

    def reposition(self, key, mark, mark2, **a):
        "handle:render:reposition"
        if mark and mark2:
            self.vstart = mark.dup()
            self.vend = mark2.dup()
            if (not self.helper_attached and
                not self.call("doc:notify:spell:mark-checked")):
                self.call("doc:attach-helper", autospell_attach_helper)
                self.helper_attached = True
            self.sched()
        return edlib.Efallthrough

    def sched(self):
        if not self.scheduled:
            self.scheduled = True
            self.call("event:timer", 10, self.rescan)

    def rescan(self, key, focus, **a):
        self.scheduled = False
        if not self.vstart or not self.vend:
            return edlib.Efalse
        start = self.vstart.dup()
        end = self.vend.dup()
        self.call("doc:notify:spell:choose-range", start, end)
        if start >= end:
            # nothing to do
            return edlib.Efail

        focus = focus.leaf

        remain = 20
        ch = None
        while start < end and remain > 0:
            remain -= 1
            ed = start.dup()
            focus.call("Spell:NextWord", ed)
            st = ed.dup()
            word = focus.call("Spell:ThisWord", ed, st, ret='str')
            self.call("doc:notify:spell:mark-checked", start, ed)
            start = ed
            if word:
                ret = focus.call("Spell:Check", word)
                if ret < 0:
                    # definite error: mark it
                    focus.call("doc:set-attr", st, "render:spell-incorrect",
                               "%d" % len(word))
                else:
                    focus.call("doc:set-attr", st, "render:spell-incorrect",
                               None);
            else:
                remain = -1
        if remain >= 0:
            self.sched()
        return edlib.Efail

def autospell_attach(key, focus, comm2, **a):
    p = autospell_view(focus)
    if comm2:
        comm2("callback", p)
    return 1

def autospell_attach_helper(key, focus, **a):
    p = autospell_monitor(focus)
    return 1;

def autospell_activate(key, focus, comm2, **a):
    autospell_view(focus)

    v = focus['view-default']
    if v:
        v = v + ',autospell'
    else:
        v = 'autospell'
    focus.call("doc:set:view-default", v)

    return 1

editor.call("global-set-command", "attach-autospell", autospell_attach)
editor.call("global-set-command", "interactive-cmd-autospell",
            autospell_activate)
