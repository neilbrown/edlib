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

def remove_range(focus, viewnum, attr, start, end):
    m = focus.call("doc:vmark-get", viewnum, start, 3, ret='mark2')
    if m and not m[attr]:
        # immediately after start is not active, so the earlist we might need
        # to remove is the next mark, or possibly the very first
        if m:
            m = m.next()
        else:
            m = focus.call("doc:vmark-get", viewnum, ret='mark')
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
    m2 = focus.call("doc:vmark-get", viewnum, end, 3, ret='mark2')
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
    m1 = focus.call("doc:vmark-get", viewnum, start, 3, ret='mark2')
    if m1 and m1[attr]:
        m1 = m1.next()
        # can move m1 down as needed
    elif m1 and m1 == start:
        # can move m1 down
        pass
    else:
        m1 = None
        # must create new mark, or move a later mark up
    m2 = focus.call("doc:vmark-get", viewnum, end, 3, ret='mark2')
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
    m1 = focus.call("doc:vmark-get", viewnum, start, 3, ret='mark2')
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
        m2 = focus.call("doc:vmark-get", viewnum, ret='mark')
    if m2 and m2 < end:
        end.to_mark(m2)

class autospell(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        # visible region
        self.vstart = None
        self.vend = None
        # checked marks
        self.view = self.call("doc:add-view") - 1
        self.call("doc:request:doc:replaced")
        self.scheduled = False
        # trigger render-lines refresh notification
        pt = focus.call("doc:point", ret='mark')
        # This hack causes render:reposition to be resent.
        focus.call("Move-View-Pos", pt)

    def handle_close(self, key, **a):
        "handle:Close"
        m = self.call("doc:vmark-get", self.view, ret='mark')
        while m:
            m.release()
            m = self.call("doc:vmark-get", self.view, ret='mark')
        self.call("doc:del-view", self.view)
        self.vstart = None
        self.vend = None

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = autospell(focus)
        self.clone_children(p)
        return 1

    def handle_clip(self, key, mark, mark2, num, **a):
        "handle:Notify:clip"
        self.clip(self.view, mark, mark2, num)
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

    def reposition(self, key, mark, mark2, **a):
        "handle:render:reposition"
        if mark and mark2:
            self.vstart = mark.dup()
            self.vend = mark2.dup()
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
        done = self.call("doc:vmark-get", self.vstart, self.view, 3, ret='mark2')
        if done:
            if done['spell:start']:
                done = done.next()
        start = self.vstart.dup()
        end = self.vend.dup()
        choose_range(self, self.view, 'spell:start', start, end)
        if start >= end:
            # nothing to do
            return edlib.Efail

        remain = 20
        ch = None
        while start < end and remain > 0:
            remain -= 1
            m = start.dup()
            focus.call("Move-WORD", m, 1)
            focus.call("Move-Char", m, 1)
            st = m.dup()
            focus.call("Move-WORD", st, -1)
            ed = st.dup()
            focus.call("Move-WORD", ed, 1)
            # discard non-alpha before and after
            ch = focus.following(st)
            while st < ed and ch and not ch.isalpha():
                focus.next(st)
                ch = focus.following(st)
            ch = focus.prior(ed)
            while ed > st and ch and not ch.isalpha():
                focus.prev(ed)
                ch = focus.prior(ed)
            # get the word.  If not empty, this starts and ends
            # with alpha and might contain puctuation.  Apostrophies
            # are good, periods might be good.  Hyphens are probably
            # bad.  Need to clean this up more. FIXME
            word = focus.call("doc:get-str", st, ed, ret='str')
            if ed > m:
                m.to_mark(ed)
            ch = focus.next(m)
            add_range(self, self.view, 'spell:start', start, m)
            start = m
            if ch == None:
                remain = 0
            if word:
                ret = focus.call("SpellCheck", word)
                if ret < 0:
                    # definite error: mark it
                    focus.call("doc:set-attr", st, "render:spell-incorrect",
                               "%d" % len(word))
                else:
                    focus.call("doc:set-attr", st, "render:spell-incorrect",
                               None);
        self.sched()
        return edlib.Efail

    def handle_replace(self, key, focus, mark, mark2, num2, **a):
        "handle:doc:replaced"
        if num2:
            # only atts changed
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

        self.sched()
        return 1

def autospell_attach(key, focus, comm2, **a):
    p = autospell(focus)
    if comm2:
        comm2("callback", p)
    return 1

def autospell_activate(key, focus, comm2, **a):
    autospell(focus)

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
