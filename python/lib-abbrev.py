# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019-2020 <neil@brown.name>
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

class AbbrevPane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

        self.call("doc:request:doc:replaced")
        self.call("doc:request:point:moving")
        self.active = False
        p = focus.call("doc:point", ret='mark')
        self.prefix_end = p.dup()
        m = p.dup()
        focus.call("doc:step", m, 0, 1)
        try:
            focus.call("text-search", 0, 1, "\\<", m)
        except edlib.commandfailed:
            pass

        self.prefix_start = m
        self.prefix = focus.call("doc:get-str", m, p, ret='str')
        self.prefix_len = len(self.prefix)

        self.prefix_start['render:abbrev'] = 'prefix'
        self.prefix_end['render:abbrev'] = 'completion'
        self.prefix_end.step(0)
        self.completions = []
        self.current = -1
        self.next_completion(1)

    def get_completions(self):
        m = self.prefix_start.dup()
        self.call("doc:step", 1, 1, m)
        again = True
        patn = "\\<"
        for c in self.prefix:
            if c in "\\()|[.*^$":
                patn += "\\"
            patn += c
        # prefer matches just after point - skip forward 100 chars
        self.call("Move-Char", m, 100)
        start = m.dup()
        while again:
            if self.call("doc:step", 0, 1, m, ret='char') is None:
                break
            try:
                l = self.call("text-search", patn, 1, -1, m)
                again = True
                l -= 1
            except edlib.commandfailed:
                again = False
            if again and m != self.prefix_start:
                e = m.dup()
                while l > 0:
                    self.call("doc:step", 1, 1, e)
                    l -= 1
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
            if p.call("doc:step", 1, 1, m, ret='char') is None:
                break
            try:
                l = p.call("text-search", self.patn, 1, 0, m)
                again = True
                l -= 1
            except edlib.commandfailed:
                again = False
            if again:
                e = m.dup()
                while l > 0:
                    p.call("doc:step", m, 0, 1)
                    l -= 1
                try:
                    p.call("text-search", "\\>", e)
                except edlib.commandfailed:
                    pass
                if e <= m:
                    p.call("doc:step", 1, 1, m)
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
            return 1
        if focus['doc-name'] == self['doc-name']:
            return 0
        if "text" not in focus["doc-type"]:
            return 0
        self.docs_scanned += 1
        self.gather_completions(focus, None)
        return 0



    def next_completion(self, dir):
        if self.current < 0:
            self.get_completions()
        if not self.completions:
            self.complete_len = 0
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

    def handle_highlight(self, key, focus, mark, str, str2, comm2, **a):
        "handle:map-attr"
        if not comm2:
            return
        if str == "render:abbrev" and str2 == 'prefix' and mark == self.prefix_start:
            comm2("cb", focus, mark, "bg:yellow", self.prefix_len)
            return 0
        if str == "render:abbrev" and str2 == 'completion' and mark == self.prefix_end:
            comm2("cb", focus, mark, "bg:cyan", self.complete_len)
            return

    def repeat(self, key, focus, **a):
        "handle:attach-abbrev"
        self.next_completion(1)
        return 1

    def left_right(self, key, focus, mark, **a):
        "handle-list/K:Left/K:Right"
        m = self.prefix_start.dup()
        try:
            if key == "K:Left":
                self.call("doc:step", 0, 1, m)
                self.call("text-search", 0, -1, m, "\\<")
            else:
                self.call("doc:step", 1, 1, m)
                self.call("text-search", m, "\\<", self.prefix_end)
                if m >= self.prefix_end:
                    self.call("doc:step", 0, 1, m)
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
        if key == "K:Up":
            self.next_completion(-1)
        else:
            self.next_completion(1)
        return 1

    def handle_escape(self, key, focus, mark, **a):
        "handle:K:ESC"
        # remove current completion, and abort
        self.active = True
        self.call("Replace", "", self.prefix_end)
        self.active = False
        self.close()
        return 1

    def delayed_close(self, key, **a):
        # FIXME this should be automatic
        self.close()
        return 1

    def handle_activity(self, key, focus, **a):
        "handle-list/doc:replaced/point:moving/pane:defocus"
        if not self.active:
            self.call("view:changed", self.prefix_start, self.prefix_end)
            self.call("editor-on-idle", self.delayed_close)
        return 0


def abbrev_attach(key, focus, comm2, **a):
    p = AbbrevPane(focus)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("cb", p)
    return 1

editor.call("global-set-command", "attach-abbrev", abbrev_attach)
