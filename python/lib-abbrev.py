# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

# Completion of abbreviations.
# The main editor can request this pane, at which point we complete
# the current "word" from surrounding text.
# The chosen prefix is highlighted as is the chosen completion
# We then capture all keystrokes:
#   up/down move the completion through options found
#   left/right move the start-of-prefix to word boundaries
#   "attach:abbrev" moves to next completion
#   Anything else is passed to parent, and if that doesn't result
#     in a callback, we self-destruct

import edlib

class AbbrevPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.menu = None
        self.opening_menu = False

        self.call("doc:request:doc:replaced")
        self.call("doc:request:mark:moving")
        self.activate()

    def activate(self):
        self.active = False
        p = self.call("doc:point", ret='mark')
        self.prefix_end = p.dup()
        m = p.dup()
        self.prev(m)
        try:
            self.call("text-search", 0, 1, "\\<", m)
        except edlib.commandfailed:
            pass

        self.prefix_start = m
        self.prefix = self.call("doc:get-str", m, p, ret='str')
        self.prefix_len = len(self.prefix)

        self.prefix_start['render:abbrev'] = 'prefix'
        self.prefix_end['render:abbrev'] = 'completion'
        self.prefix_end.step(0)
        self.completions = []
        self.current = -1
        #self.next_completion(1)
        self.get_completions()
        self.complete_len = 0

        if not self.completions:
            return 1
        if len(self.completions) == 1:
            self.complete_with(self.completions[0])
            return
        # find longest prefix
        p = self.completions[0]
        for i in range(1, len(self.completions)):
            c = self.completions[i]
            while p:
                if c.startswith(p):
                    break
                p = p[:-1]
            if not p:
                break
        if p:
            self.complete_with(p)
            return
        # Need a menu
        self.opening_menu = True
        mp = self.call("attach-menu", (self.parent.cx, self.parent.cy), ret='pane')
        for c in self.completions:
            mp.call("menu-add", self.prefix+c)
        mp.call("doc:file", -1)
        self.menu = mp
        self.add_notify(mp, "Notify:Close")
        self.opening_menu = False

    def handle_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.menu:
            self.menu = None
        return 1

    def complete_with(self, str):
        self.call("doc:replace", str, self.prefix_end, self.prefix_end)
        self.call("view:changed", self.prefix_start, self.prefix_end)

    def menu_done(self, key, focus, str, **a):
        "handle:menu-done"
        if not self.prefix_start:
            return
        if not str:
            # Menu aborted
            self.call("view:changed", self.prefix_start, self.prefix_end)
            self.prefix_start = None
            self.prefix_end = None
            self.call("Message", "")
            return 1
        self.call("doc:replace", str, self.prefix_end, self.prefix_start)
        self.call("view:changed", self.prefix_start, self.prefix_end)
        return 1

    def get_completions(self):
        m = self.prefix_start.dup()
        self.next(m)
        again = True
        patn = "\\<(?%d:%s)" % (len(self.prefix), self.prefix)
        # prefer matches just after point - skip forward 100 chars
        self.call("Move-Char", m, 100)
        start = m.dup()
        while again:
            if self.prev(m) is None:
                break
            try:
                l = self.call("text-search", patn, 1, -1, m)
                again = True
                l -= 1
            except edlib.commandfailed:
                again = False
            if again and m != self.prefix_start:
                e = m.dup()
                self.call("Move-Char", e, l)
                try:
                    self.call("text-search", "\\>", e)
                except edlib.commandfailed:
                    pass
                s = self.call("doc:get-str", m, e, ret='str')
                if (s.lower().startswith(self.prefix.lower()) and
                    s.lower() != self.prefix.lower()):
                    com = s[self.prefix_len:]
                    if com not in self.completions:
                        self.completions.append(com)

        # now try search forward FIXME should this be async?
        m.to_mark(start)
        self.patn = patn
        self.gather_completions(self, m)
        self.docs_scanned = 0
        self.call("docs:byeach", self.each_doc)
        if not self.completions:
            self.call("Message", "No completions found")
        elif len(self.completions) == 1:
            self.call("Message", "1 completion found")
        else:
            self.call("Message", "%d completions found" % len(self.completions))

    def gather_completions(self, p, m):
        if not m:
            m = edlib.Mark(p)

        again = True
        while again:
            if p.next(m) is None:
                break
            try:
                l = p.call("text-search", self.patn, 1, 0, m)
                again = True
                l -= 1
            except edlib.commandfailed:
                again = False
            if again:
                e = m.dup()
                p.call("Move-Char", -l, m)
                try:
                    p.call("text-search", "\\>", e)
                except edlib.commandfailed:
                    pass
                if e <= m:
                    p.next(m)
                    continue
                s = p.call("doc:get-str", m, e, ret='str')
                if (s.lower().startswith(self.prefix.lower()) and
                    s.lower() != self.prefix.lower()):
                    com = s[self.prefix_len:]
                    if com not in self.completions:
                        self.completions.append(com)
                m.to_mark(e)

    def each_doc(self, key, focus, **a):
        if self.docs_scanned > 5:
            # already handle 5 docs - stop now
            return False
        if focus['doc-name'] == self['doc-name']:
            return 1
        if "text" not in focus["doc-type"]:
            return 1
        self.docs_scanned += 1
        self.gather_completions(focus, None)
        return 1

    def next_completion(self, dir):
        if self.current < 0:
            self.get_completions()
        if not self.completions:
            self.complete_len = 0
            # remove any previous completion
            self.active = True
            self.call("Replace", self.prefix_end)
            self.active = False
        else:
            self.current += dir
            if self.current < 0:
                self.current = len(self.completions) - 1
            elif self.current >= len(self.completions):
                self.current = 0
            c = self.completions[self.current]
            self.active = True
            self.call("Replace", c, self.prefix_end)
            self.active = False
            self.complete_len = len(c)
        self.call("view:changed", self.prefix_start, self.prefix_end)

    def handle_draw(self, key, focus, str2, xy, **a):
        "handle:Draw:text"
        if not self.menu or not str2 or ",menu_here" not in str2:
            return edlib.Efallthrough

        p = self.menu.call("ThisPopup", ret='pane')
        if p:
            xy = p.parent.mapxy(focus, xy[0], focus.h)
            p.x = xy[0]
            p.y = xy[1]
        return edlib.Efallthrough

    def handle_highlight(self, key, focus, mark, str1, str2, xy, comm2, **a):
        "handle:map-attr"
        if not comm2 or not self.prefix_start:
            return
        if str1 == "render:abbrev" and str2 == 'prefix' and mark == self.prefix_start:
            comm2("cb", focus, mark, "bg:yellow", self.prefix_len, 250)
            comm2("cb", focus, mark, "menu_here", 1, 250)
            return 1
        if str1 == "render:abbrev" and str2 == 'completion' and mark == self.prefix_end:
            comm2("cb", focus, mark, "bg:cyan", self.complete_len, 250)
            return 1

    def repeat(self, key, focus, **a):
        "handle:attach-abbrev"
        if not self.prefix_start:
            self.activate()
        else:
            self.next_completion(1)
        return 1

    def left_right(self, key, focus, mark, **a):
        "handle-list/K:Left/K:Right"
        if not self.prefix_start:
            return edlib.Efallthrough

        m = self.prefix_start.dup()
        try:
            if key == "K:Left":
                self.prev(m)
                self.call("text-search", 0, -1, m, "\\<")
            else:
                self.next(m)
                self.call("text-search", m, "\\<", self.prefix_end)
                if m >= self.prefix_end:
                    self.prev(m)
        except edlib.commandfailed:
            return 1
        self.prefix_start.to_mark(m)
        self.prefix = focus.call("doc:get-str", m, self.prefix_end, ret='str')
        self.prefix_len = len(self.prefix)
        self.completions = []
        self.current = -1
        self.next_completion(1)
        return 1

    def up_down(self, key, focus, mark, **a):
        "handle-list/K:Up/K:Down"
        if not self.prefix_start:
            return edlib.Efallthrough
        if key == "K:Up":
            self.next_completion(-1)
        else:
            self.next_completion(1)
        return 1

    def handle_escape(self, key, focus, mark, **a):
        "handle:K:ESC"
        if not self.prefix_start:
            return edlib.Efallthrough
        # remove current completion, and abort
        self.active = True
        self.call("Replace", "", self.prefix_end)
        self.active = False
        self.call("view:changed", self.prefix_start, self.prefix_end)
        self.prefix_start = None
        self.prefix_end = None
        self.call("Message", "")
        return 1

    def handle_activity(self, key, focus, mark, **a):
        "handle-list/doc:replaced/mark:moving/pane:defocus"
        if key == "mark:moving":
            point = self.call("doc:point", ret = 'mark')
            if mark.seq != point.seq:
                return edlib.Efallthrough

        if key == "pane:defocus":
            if self.opening_menu:
                # Focus moved to menu - ignore
                return edlib.Efallthrough
            if self.has_focus():
                # Maybe the menu lost focus ... I wonder why I would care
                return edlib.Efallthrough

        if not self.active and self.prefix_start:
            self.call("view:changed", self.prefix_start, self.prefix_end)
            self.prefix_start = None
            self.prefix_end = None
            #self.call("Message", "")
        return edlib.Efallthrough

def abbrev_attach(key, focus, comm2, **a):
    p = AbbrevPane(focus)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("cb", p)
    return 1

edlib.editor.call("global-set-command", "attach-abbrev", abbrev_attach)
