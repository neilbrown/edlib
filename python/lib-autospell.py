# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2021-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# autospell: spell-check visible words in a document and highlight
#    those that aren't in a dictionary
#

# We keep a list of marks which alternate between the start and end
# of a "checked" section of text.  The 'start' marks have an attribute
# ('spell:start') set.  The 'end' marks do not.
# We remove ranges from this list when the document is changed.  This
# might require splitting a 'checked' section, or removing some sections
# completely.
# We add ranges when a word is checked.  This might merge with existing
# ranges.
# We need to find an unchecked section in some range to start checking.
# This is all managed with the help of rangetrack

import edlib
import os

class autospell_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.scheduled = False
        self.helper_attached = False
        # visible region
        self.vstart = None
        self.vend = None
        self.menu = None
        self.call("doc:request:rangetrack:recheck-autospell")
        self.call("doc:request:doc:replaced")
        self.call("doc:request:spell:dict-changed")
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
            self.vstart.clip(mark, mark2, num)
        if self.vend:
            self.vend.clip(mark, mark2, num)
        return edlib.Efallthrough

    def map_attr(self, key, focus, str1, str2, mark, comm2, **a):
        "handle:map-attr"
        if not str1 or not mark or not comm2:
            return edlib.Efallthrough
        if str1 == "render:spell-incorrect":
            comm2("cb", focus, int(str2), mark,
                  "fg:red-80,underline,action-menu:autospell-menu", 120)
        return edlib.Efallthrough

    def handle_click(self, key, focus, mark, xy, str1, **a):
        "handle:autospell-menu"
        if self.menu:
            self.menu.call("Cancel")
        mp = self.call("attach-menu", "", "autospell-choice", xy, ret='pane')
        self.wordend = mark.dup()
        st = mark.dup()
        w = focus.call("Spell:ThisWord", focus, mark, st, ret='str')
        self.thisword = w
        mp.call("menu-add", "[Insert in dict]", "+")
        mp.call("menu-add", "[Accept for now]", "!")
        focus.call("Spell:Suggest", w,
                   lambda key, str1, **a: mp.call("menu-add", str1))
        mp.call("doc:file", -1)
        self.menu = mp
        self.add_notify(mp, "Notify:Close")
        return 1

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.menu:
            self.menu = None
            return 1
        return edlib.Efallthrough

    def handle_choice(self, key, focus, mark, str1, **a):
        "handle:autospell-choice"
        if not str1:
            return None
        m = self.wordend
        self.wordend = None
        st = m.dup()
        w = focus.call("Spell:ThisWord", m, st, ret='str')
        if str1 == "+":
            focus.call("Spell:AddWord", 1, self.thisword)
            focus.call("spell:dict-changed", st, m)
            return 1
        if str1 == '!':
            focus.call("Spell:AddWord", 0, self.thisword)
            focus.call("spell:dict-changed", st, m)
            return 1
        if w == self.thisword:
            focus.call("doc:replace", st, m, str1)
        return 1

    def handle_recheck(self, key, **a):
        "handle:rangetrack:recheck-autospell"
        self.sched()
        return 1

    def handle_dict_changed(self, key, focus, mark, mark2, num2, **a):
        "handle:spell:dict-changed"
        # clear everything
        self.call("doc:notify:rangetrack:clear", "autospell")
        return 1

    def handle_replace(self, key, focus, mark, mark2, num2, **a):
        "handle:doc:replaced"
        if not mark or not mark2:
            # clear everything
            self.call("doc:notify:rangetrack:clear", "autospell")
            return 1

        # mark2 might have been the start-of-word, but not any longer
        # So any spell-incorrect must be cleared as normal checking
        # only affects first char of a word.
        focus.call("doc:set-attr", mark2, "render:spell-incorrect",
                   None);

        # if change at either end of view, extend view until reposition message
        # If we don't get a render:resposition message then probably the
        # rendered didn't see the mark move, but we did, because the change
        # was between the marks?  Should I test more precisely for that FIXME
        if mark < self.vstart and mark2 >= self.vstart:
            self.vstart.to_mark(mark)
        if mark2 > self.vend and mark <= self.vend:
            self.vend.to_mark(mark2)

        # Need to capture adjacent words, and avoid zero-size gap
        mark = mark.dup()
        focus.prev(mark)
        mark2 = mark2.dup()
        focus.next(mark2)
        self.call("doc:notify:rangetrack:clear", "autospell", mark, mark2)

        return 1

    def reposition(self, key, mark, mark2, **a):
        "handle:render:reposition"
        if mark and mark2:
            self.vstart = mark.dup()
            self.vend = mark2.dup()
            if (not self.helper_attached and
                self.call("doc:notify:rangetrack:add",
                              "autospell") <= 0):
                if self.call("rangetrack:new", "autospell") > 0:
                    self.helper_attached = True
                else:
                    pass # FIXME
            self.sched()
        return edlib.Efallthrough

    def sched(self):
        if not self.scheduled:
            self.scheduled = True
            self.call("event:on-idle", self.rescan)

    def rescan(self, key, focus, **a):
        self.scheduled = False
        if not self.vstart or not self.vend:
            return edlib.Efalse
        start = self.vstart.dup()
        end = self.vend.dup()
        self.call("doc:notify:rangetrack:choose", "autospell",
                  start, end)
        if start >= end:
            # nothing to do
            return edlib.Efalse

        self.set_time()
        focus = focus.final_focus

        if edlib.testing:
            remain = 20
        else:
            remain = 200
        ch = None
        while start < end and remain > 0 and not self.too_long():
            remain -= 1
            ed = start.dup()
            focus.call("Spell:NextWord", ed)
            st = ed.dup()
            word = focus.call("Spell:ThisWord", ed, st, ret='str')
            self.call("doc:notify:rangetrack:add", "autospell",
                      start, ed)
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
        return edlib.Efalse

def autospell_attach(key, focus, comm2, **a):
    p = autospell_view(focus)
    if comm2:
        comm2("callback", p)
    return 1

def autospell_activate(key, focus, comm2, **a):
    autospell_view(focus)

    focus.call("doc:append:view-default", ",autospell")

    return 1

edlib.editor.call("global-set-command", "attach-autospell", autospell_attach)
edlib.editor.call("global-set-command", "interactive-cmd-autospell",
                  autospell_activate)
