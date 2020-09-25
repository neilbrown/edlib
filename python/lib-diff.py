# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019-2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

# This pane overlays a pane showing unified diff output and:
# - colourizes + and - lines
# - interprets ':Enter' to find the given line

import os.path


def djoin(dir, tail):
    # 'tail' might not exist at 'dir', but might exist below some
    # prefix of dir.  We want to find that prefix and add it.
    orig = dir
    while dir:
        p = os.path.join(dir, tail)
        if os.path.exists(p):
            return p
        d = os.path.dirname(dir)
        if not d or d == dir:
            break
        dir = d
    return os.path.join(orig, tail)

def measure(p, start, end):
    start = start.dup()
    len = 0
    while start < end:
        if p.call("Move-Char", start, 1) <= 0:
            break
        len += 1
    return len

class DiffPane(edlib.Pane):
    def __init__(self, focus, which = 0):
        edlib.Pane.__init__(self, focus)
        self.setwhich(which)
        self.viewnum = focus.call("doc:add-view", self) - 1

    def setwhich(self, which):
        self.which = which
        if which == 0:
            st = "(post)"
        elif which == 1:
            st = "(pre)"
        else:
            st = "(pre-%d)" % which
        self['doc-status'] = st
        self.call("view:changed")

    def handle_close(self, key, focus, **a):
        "handle:Close"
        m = self.call("doc:vmark-get", self.viewnum, ret='mark')
        while m:
            m.release()
            m = self.call("doc:vmark-get", self.viewnum, ret='mark')
        self.call("doc:del-view", self.viewnum)

    def handle_next(self, key, focus, mark, **a):
        "handle-list/K:M-p/K:Prior"
        # Find previous diff hunk
        edlib.LOG("prev")
        try:
            focus.call("text-search", 0, 1, "^([^-+]|$)", mark)
            focus.call("text-search", 0, 1, "^[-+]", mark)
        except edlib.commandfailed:
            edlib.LOG("failed")
            pass
        return 1

    def handle_prev(self, key, focus, mark, **a):
        "handle-list/K:M-n/K:Next"
        # Find previous diff hunk
        try:
            focus.call("text-search", 0, 0, "^([^-+]|$)", mark)
            focus.call("text-search", 0, 0, "^[-+]", mark)
        except edlib.commandfailed:
            pass
        return 1

    def handle_highlight(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2:
            return
        if str == "start-of-line":
            c = focus.call("doc:step", 1, mark, ret='char')
            if c not in '-+':
                return 0

            # check if we have highlighted the words
            m = focus.call("doc:vmark-get", self, self.viewnum, 3, mark, ret='mark2')
            if m:
                st = int(m['start'])
            else:
                st = 0
            # st = 0 or -1 if 'same's aren't marked, '1' if they are
            if st <= 0:
                if c == '+':
                    comm2("attr:cb", focus, mark,
                          "fg:green-60,bg:white,nobold", 1, 5)
                    comm2("attr:cb", focus, mark,
                          "fg:green-60,bg:cyan+90,bold", 10000, 2)
                else:
                    comm2("attr:cb", focus, mark,
                          "fg:red-60,bg:white,nobold", 1, 5)
                    comm2("attr:cb", focus, mark,
                          "fg:red-60,bg:magenta+90,bold", 10000, 2)
                if st == 0:
                    self.handle_wordwise('auto', focus, mark)
                return 0

            # Set attr for leading '+' with length 1 and high prio
            # Set attr for differing text with length LARGE and lower prio
            if c == '+':
                comm2("attr:cb", focus, mark, "fg:green-60,bg:white,nobold", 1, 5)
                comm2("attr:cb", focus, mark, "fg:green-60,bg:cyan+90,bold", 10000, 2)
            elif c == '-':
                comm2("attr:cb", focus, mark, "fg:red-60,bg:white,nobold", 1, 5)
                comm2("attr:cb", focus, mark, "fg:red-60,bg:magenta+90,bold", 10000, 2)

            return 0
        if str == "render:diff-same":
            w = str2.split()
            len = int(w[0])
            if w[1] == '1':
                # This is the '+' section
                comm2("attr:cb", focus, mark, "fg:green-60,bg:white,nobold", len, 3)
            else:
                comm2("attr:cb", focus, mark, "fg:red-60,bg:white,nobold", len, 3)
            return 0

    def handle_wordwise(self, key, focus, mark, **a):
        "handle:doc:cmd-w"
        if not mark:
            return
        mark = mark.dup()
        found = False

        focus.call("Move-EOL", mark, -1)
        ch = focus.call("doc:step", 1, mark, ret='char')
        m = mark.dup()
        while ch in '-+':
            mark.to_mark(m)
            if focus.call("Move-EOL", m, -2) <= 0:
                break
            ch = focus.call("doc:step", 1, m, ret='char')

        is_hunk = ch == ' ' or ch == '@'

        starta = mark.dup()
        ch = focus.call("doc:step", 1, mark, ret='char')
        while ch and ch == '-':
            if (focus.call("Move-EOL", mark, 1) <= 0 or
                focus.call("Move-Char", mark, 1) <= 0):
                break
            ch = focus.call("doc:step", 1, mark, ret='char')
        startb = mark.dup()
        ch = focus.call("doc:step", 1, mark, ret='char')
        while ch and ch == '+':
            if (focus.call("Move-EOL", mark, 1) <= 0 or
                focus.call("Move-Char", mark, 1) <= 0):
                break
            ch = focus.call("doc:step", 1, mark, ret='char')

        alen = measure(focus, starta, startb)
        blen = measure(focus, startb, mark)
        if alen == 0 or blen == 0 or not is_hunk:
                msg = "Nothing to compare here!"
                ret = 4
        else:
            cmd = focus.call("MakeWiggle", ret='comm')
            if not cmd:
                return edlib.Efail
            cmd("before", focus, starta, startb, 1)
            cmd("after", focus, startb, mark, 1)
            ret = cmd("set-common", focus, "render:diff-same")
            del cmd
            focus.call("view:changed", starta, mark)
        if key == 'auto':
            m1 = edlib.Mark(self, self.viewnum)
            if ret in [2, 3]:
                m1['start'] = '1'
            else:
                m1['start'] = '-1'
            m2 = edlib.Mark(self, self.viewnum)
            m2['start'] = '0'
            m1.to_mark(starta)
            m2.to_mark(mark)

            return 1

        if ret == 4:
            pass
        elif ret == 1:
            msg = "No difference found"
        elif ret == 2:
            msg = "Only white-space differences found"
        elif ret == 3:
            msg = "Common text has been highlighted"
        else:
            msg = "WordDiff failed"
        focus.call("Message", msg)
        return 1

    def handle_clone(self, key, focus, home, **a):
        "handle:Clone"
        p = DiffPane(focus)
        home.clone_children(p)

    def handle_toggle(self, key, focus, **a):
        "handle:doc:cmd-~"
        if self.which == 0:
            self.setwhich(1)
        else:
            self.setwhich(0)
        return 1

    def handle_enter(self, key, focus, mark, **a):
        "handle:K:Enter"
        m = mark.dup()
        focus.call("Move-EOL", -1, m)

        ptn = "^(@@+)( -([\\d]+),([\\d]+))+ \\+([\\d]+),([\\d]+)"
        try:
            focus.call("text-search", m, ptn, 0, 1)
        except edlib.commandfailed:
            focus.call("Message", "Not on a diff hunk - no '@@' line")
            return 1
        cmd = focus.call("make-search", ptn, 3, ret='comm')
        m2 = m.dup()
        focus.call("doc:content", m2, cmd)
        f = cmd("getcapture", "len", focus, 1)-1
        if self.which == 0 or self.which > f:
            # get the "after" section
            lineno = cmd("interp", focus, "\\5", ret='str')
            lines = cmd("interp", focus, "\\6", ret='str')
        else:
            # choose a "before" section
            lineno = cmd("interp", focus, "\\:3:%d" % self.which, ret='str')
            lines = cmd("interp", focus, "\\:4:%d" % self.which, ret='str')

        wcmd = focus.call("MakeWiggle", ret='comm')
        if not wcmd:
            return edlib.Efail
        focus.call("Move-EOL", 1, m2); focus.call("Move-Char", 1, m2)
        if self.which == 0 or self.which >= f:
            wcmd("after", focus, m2, mark, f-1, f)
        else:
            wcmd("after", focus, m2, mark, f-1, self.which)
        prefix = wcmd("extract", focus, "after", ret='str')
        if self.which == 0 or self.which >= f:
            wcmd("after", focus, m2, lines, f-1, f)
        else:
            wcmd("after", focus, m2, lines, f-1, self.which)

        from_start = len(prefix.splitlines())

        # need to find a line starting '+++' immediately before
        # one starting '@@+'
        try:
            focus.call("text-search", "^\\+\\+\\+.*\\n@@", m, 0, 1)
        except edlib.commandfailed:
            focus.call("Message", "Not on a diff hunk! No +++ line found")
            return 1
        ms = m.dup()
        focus.call("Move-EOL", 1, m)
        fname = focus.call("doc:get-str", ms, m, ret='str')
        fname = fname.lstrip('+ ')

        # quilt adds timestamp info after a tab
        tb = fname.find('\t')
        if tb > 0:
            fname = fname[:tb]

        # git and other use a/ and b/ rather than actual directory name
        if fname.startswith('b/') or fname.startswith('a/'):
            fname = fname[2:]
        if fname[0] != '/':
            fname = djoin(focus['dirname'], fname)
        lineno = int(lineno) + from_start
        try:
            d = focus.call("doc:open", -1, 8, fname, ret='focus')
        except edlib.commandfailed:
            d = None
        if not d:
            focus.call("Message", "File %s not found" % fname)
            return edlib.Efail

        par = focus.call("DocLeaf", d, ret='focus')
        if not par:
            par = focus.call("OtherPane", d, ret='focus')
            if not par:
                focus.call("Message", "Failed to open pane")
                return edlib.Efail
            par = d.call("doc:attach-view", par, 1, ret='focus')
        par.take_focus()
        par.call("Move-File", -1)
        if lineno > 1:
            par.call("Move-EOL", lineno - 1)
            par.call("Move-Char", 1)
            m = par.call("doc:dup-point", 0, -2, ret='mark')
            try:
                fuzz = wcmd("find", "after", 5, 200, par, m)
                from_start -= (fuzz - 1)
                par.call("Move-to", m)
                if from_start > 0:
                    par.call("Move-EOL", from_start)
                    par.call("Move-Char", 1)
                if fuzz > 1:
                    focus.call("Message", "Match found with fuzz of %d" % (fuzz-1))
            except edlib.commandfailed:
                focus.call("Message", "Couldn't find exact match")
                pass

        return 1

def diff_view_attach(key, focus, comm2, **a):
    p = focus.call("attach-viewer", ret='focus')
    p = DiffPane(p)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

def add_diff(key, focus, **a):
    p = DiffPane(focus)
    if p:
        p.call("view:changed")
    focus.call("doc:set:view-default", "diff")
    return 1


editor.call("global-set-command", "attach-diff", diff_view_attach)
editor.call("global-set-command", "interactive-cmd-diff-mode", add_diff)
editor.call("global-load-module", "lib-worddiff")
