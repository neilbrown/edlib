# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# autospell: spell-check visible words in a document and highlight
#    those that aren't in a dictionary
#

# Fix me: we aren't spell-checking the word immediately after a change
#     or new text as typed, though sometimes it works

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
        if not done or done < self.vstart:
            # vstart not within a 'done' region, so create a new
            # empty region and extend the 'end' forward
            done = edlib.Mark(focus, view = self.view, owner=self)
            done.to_mark(self.vstart)
            done['spell:start'] = 'yes'
            done = edlib.Mark(orig=done, owner=self)
            done.step(1)
        # 'done' is now an end-of-done-region marker that is not before vstart
        remain = 20
        ch = None
        next_done = done.next()
        while done < self.vend and (not next_done or
                                    done <= next_done) and remain > 0:
            remain -= 1
            focus.call("Move-WORD", done, 1)
            focus.call("Move-Char", done, 1)
            st = done.dup()
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
            if ed > done:
                done.to_mark(ed)
            ch = focus.next(done)
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
        if next_done and next_done <= done:
            # joined with next "done" region
            next_done.release()
            done.release()
            done = None
            self.sched()
        elif done <= self.vend and ch:
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

        d = self.call("doc:vmark-get", self.view, mark, 3, ret='mark2')
        if not d or not d['spell:start']:
            # just after 'mark' is not done so the earliest we might need
            # to clear is the next or first mark
            if d:
                d = d.next()
            else:
                d = self.call("doc:vmark-get", self.view, ret='mark')
            if not d or d > mark2:
                # nothing to clear
                return 1
            # d is now the start of a 'done' section that in within mark-mark2
            # and should should be cleared
        else:
            # from d to mark are done and should stay that way.
            d = edlib.Mark(focus, view = self.view, owner=self)
            d.to_mark(mark)
            d = edlib.Mark(orig=d, owner=self)
            d.step(1)
            d['spell:start'] = 'yes'
            # d is start of a newly-split 'done' that must be cleared
        d2 = self.call("doc:vmark-get", self.view, mark2, 3, ret='mark2')
        if d2 and d2 == mark2 and d2['spell:start']:
            # this done section is entirely after mark2, so not interesting
            d2 = d2.prev()
        if d2 and d2['spell:start']:
            # mark2 is within a 'done' region that needs to be split
            d2 = edlib.Mark(focus, view = self.view, owner=self)
            d2.to_mark(mark2)
            d2['spell:start'] = 'yes'
            d2 = edlib.Mark(orig=d2, owner=self)
            d2.step(0)

        # d2 is now the end of a done region that needs to be discarded
        done = self.call("doc:vmark-get", self.view, ret='mark')
        while d < d2:
            o = d
            d = d.next()
            o.release()
        d2.release()
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
