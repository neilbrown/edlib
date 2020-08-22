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
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.viewnum = focus.call("doc:add-view", self) - 1

    def handle_close(self, key, focus, **a):
        "handle:Close"
        m = self.call("doc:vmark-get", self.viewnum, ret='mark')
        while m:
            m.release()
            m = self.call("doc:vmark-get", self.viewnum, ret='mark')
        self.call("doc:del-view", self.viewnum)

    def handle_highlight(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2:
            return
        if str == "start-of-line":
            c = focus.call("doc:step", 1, mark, ret='char')
            if c == '+':
                comm2("attr:cb", focus, mark, "fg:green-40", 1000000, 2)
            elif c == '-':
                comm2("attr:cb", focus, mark, "fg:red-40", 1000000, 2)
            else:
                return 0
            # check if we have highlighted the words
            m = focus.call("doc:vmark-get", self, self.viewnum, 3, mark, ret='mark2')
            if m and m['start'] == '1':
                       return 0
            self.handle_wordwise('auto', focus, mark)
        if str == "render:diff-same":
            comm2("attr:cb", focus, mark, "bg:yellow+50,underline", int(str2), 1)
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

        if key == 'auto':
            m1 = edlib.Mark(self, self.viewnum)
            m1['start'] = '1'
            m2 = edlib.Mark(self, self.viewnum)
            m2['start'] = '0'
            m1.to_mark(starta)
            m2.to_mark(mark)

        alen = measure(focus, starta, startb)
        blen = measure(focus, startb, mark)
        if alen == 0 or blen == 0:
                msg = "Nothing to compare here!"
                ret = 4
        else:
            ret = focus.call("WordDiff", starta, alen, startb, blen,
                             "render:diff-same", 'skip')
        if key == 'auto':
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

    def handle_enter(self, key, focus, mark, **a):
        "handle:K:Enter"
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        lines = [0,0,0,0]
        while focus.call("doc:step", 0, m, ret='char') == '\n':
            m2 = m.dup()
            if focus.call("text-match", m2,
                          "@@+( -[\d]+,[\d]+)+ \+") > 0:
                break
            m2 = None
            c = focus.call("doc:step", m, 1, ret='char')
            f = 0
            # we don't know how many base files are being diffed against
            # until we see the '@@+' line, so allow for 1-4
            # If there are any '-' on a line that we need to pay attention
            # to, then we don't count that line.  We only know how many
            # chars to pay attention when we see that '@@+' line.
            while c and c in '+ ' and f < 4:
                lines[f] += 1
                f += 1
                focus.call("doc:step", m, 1, 1)
                c = focus.call("doc:step", m, 1, ret='char')
            focus.call("Move-EOL", -2, m)
        if not m2:
            focus.call("Message", "Not on a diff hunk - no '@@' line")
            return 1
        f = -2
        while f < 4 and focus.call("doc:step", m, 1, 1, ret='char') == '@':
            f += 1
        if f >= 0 and f < 4:
            lines = lines[f]
        else:
            focus.call("Message", "Not on a diff hunk - '@@' line looks wrong")
            return 1
        # m2 is after the '+' that introduces the to-file-range
        m.to_mark(m2)
        focus.call("text-match", m, "[\d]+")
        s = focus.call("doc:get-str", m2, m, ret='str')
        if len(s) == 0:
            focus.call("Message", "Not on a diff hunk! Line number is empty")
            return 1
        # need to find a line starting '+++' immediately before
        # one starting '@@+'
        at_at = True
        found_plus = False
        while not found_plus:
            if focus.call("doc:step", 0, m, ret='char') is None:
                # hit start of file without finding anything
                break
            focus.call("Move-EOL", -2, m)
            if at_at:
                at_at = False
                if focus.call("text-match", m, "\+\+\+ ") > 0:
                    found_plus = True
            else:
                if focus.call("text-match", m, "@@+ ") > 0:
                    at_at = True
        if not found_plus:
            focus.call("Message", "Not on a diff hunk! No +++ line found")
            return 1
        ms = m.dup()
        focus.call("Move-EOL", 1, m)
        fname = focus.call("doc:get-str", ms, m, ret='str')
        # quilt adds timestamp info after a tab
        tb = fname.find('\t')
        if tb > 0:
            fname = fname[:tb]

        # git and other use a/ and b/ rather than actual directory name
        if fname.startswith('b/'):
            fname = fname[2:]
        if fname[0] != '/':
            fname = djoin(focus['dirname'], fname)
        lines = int(s) + lines - 1
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
        if lines > 1:
            par.call("Move-EOL",lines - 1)
            par.call("Move-Char", 1)

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
