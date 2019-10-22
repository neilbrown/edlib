# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2018-2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

def textwidth(line):
    w = 0
    for c in line:
        if c == '\t':
            w = w | 7
        w += 1
    return w

class CModePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        # for paren-highlight
        self.pre_paren = None
        self.post_paren = None

        # for indent
        self.spaces = None   # is set to a number, use spaces, else TABs
        self.indent_type = None

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = CModePane(focus)
        p.spaces = self.spaces
        p.indent_type = self.indent_type
        self.clone_children(p)
        return 1

    def tabify(self, indent):
        if self.spaces:
            return indent
        i = 1
        while i >= 0:
            try:
                i = indent.index('        ')
            except ValueError:
                i = -1
            if i >= 0:
                indent = indent[:i] + "\t" + indent[i+8:]
        return indent

    def mkwhite(self, ii):
        (depth, align, intro) = ii
        if align == 0:
            align = depth[-1]
        rv = ''
        if not self.spaces:
            while align >= 8:
                align -= 8
                rv += '\t'
        return rv + ' ' * align

    def calc_indent_python(self, p, m):
        # like calc-indent, but check for ':' and end of previous line
        indent = self.calc_indent(p, m, 'default')
        m = m.dup()
        c = p.call("doc:step", 0, 1, m, ret='char')
        while c in ' \t\n':
            c = p.call("doc:step", 0, 1, m, ret='char')
        if c == ':':
            indent[0].append(indent[0][-1]+self.spaces)
        return indent

    def calc_indent(self, p, m, type='check'):
        # m is at the end of a line or start of next line in p - Don't move it
        # Find indent for 'next' line as a list of depths
        # and an alignment...

        if type == 'check':
            if self.indent_type == 'C':
                return self.calc_indent_c(p, m)
            if self.indent_type == 'python':
                return self.calc_indent_python(p, m)

        m = m.dup()
        # Find previous line which is not empty and make up
        # a depth list from the leading tabs/spaces.
        while p.call("doc:step", 0, m, ret="char") in ' \t':
            p.call("doc:step", 0, 1, m)
        line_start = m.dup()
        while p.call("doc:step", 0, m, ret="char") == '\n':
            p.call("doc:step", 0, 1, m)
        if p.call("doc:step", 0, m) == edlib.WEOF:
            # No previous non-empty line.
            return ([0], None)
        # line before m is not empty
        m2 = m.dup()
        # walk to start of line, leaving m2 at leading non-space
        c = p.call("doc:step", 0, m2, ret="char")
        while c != None and c != '\n':
            p.call("doc:step", 0, 1, m2)
            if c not in " \t":
                m.to_mark(m2)
            c = p.call("doc:step", 0, m2, ret="char")
        # m2 .. m is the prefix
        pfx = p.call("doc:get-str", m2, m, ret = 'str')
        w = textwidth(pfx)
        if self.spaces:
            t = self.spaces
        else:
            t = 8
        r = [0]
        while w >= t:
            w -= t
            r.append(r[-1] + t)
        if w:
            r.append(r[-1] + w)

        # Now see if there are any parentheses.
        # i.e see if a () expression started since the indent.
        # If so, either line-up with open, or add a tab if open
        # is at end-of-line
        indent_end = m
        expr = line_start.dup()
        p.call("Move-Expr", -1, 1, expr)
        if expr >= indent_end:
            p.call("doc:step", expr, 1, 1)
            if p.call("doc:step", expr, 1, 0, ret="char") == '\n':
                # open-bracket at end-of-line, so add a standard indent
                extra = t
            else:
                extra = p.call("doc:get-str", indent_end, expr, ret='str')
                extra = len(extra)
            extra += r[-1]
        else:
            extra = 0
        return (r, extra, '')

    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        # If there is white space before or after the cursor,
        # remove it.  Then work out how indented this line
        # should be, based on last non-empty line, and insert
        # that much space.
        m = mark.dup()
        # First, move point forward over any white space
        c = focus.call("doc:step", 1, 0, m, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 1, 1, m)
            c = focus.call("doc:step", 1, 0, m, ret="char")
        focus.call("Move-to", m)
        # Second, move m back over any white space.
        c = focus.call("doc:step", 0, 1, m, ret="char")
        while c != None:
            if c not in " \t":
                focus.call("doc:step", 1, 1, m)
                break
            c = focus.call("doc:step", 0, 1, m, ret="char")
        # Now calculate the indent
        indent = self.calc_indent(focus, m)

        # and replace all that white space with a newline and the indent
        return focus.call("Replace", 1, m, "\n" + self.mkwhite(indent))

    def handle_tab(self, key, focus, mark, **a):
        "handle:Tab"
        # if there is only white-space before cursor (up to newline) then:
        # move to end of white-space
        # - choose an indent as for Return
        # - If we don't have exactly that, replace with that
        # - if we do and chosen indent has no extra, add one indent level
        m = mark.dup()
        c = focus.call("doc:step", 0, 0, m, ret="char")
        prevc = c
        while c and c in " \t":
            focus.call("doc:step", 0, 1, m)
            c = focus.call("doc:step", 0, 0, m, ret="char")
        if not (c is None or c == "\n"):
            # not at start of line, maybe convert preceding spaces to tabs
            if prevc != ' ':
                return 0
            m = mark.dup()
            len = 0
            while focus.call("doc:step", 0, 0, m, ret='char') == ' ':
                len += 1
                focus.call("doc:step", 0, 1, m)
            new = "\t" * (len / 8)
            focus.call("Replace", 1, m, mark, new)
            # fall through it insert a new tab
            return 0

        # m at start-of-line, move mark (point) to first non-white-space
        c = focus.call("doc:step", 1, 0, mark, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 1, 1, mark)
            c = focus.call("doc:step", 1, 0, mark, ret="char")

        indent = self.calc_indent(focus, m)

        new = self.mkwhite(indent)

        current = focus.call("doc:get-str", m, mark, ret="str")

        if new != current:
            return focus.call("Replace", 1, m, mark, new)
        if indent[1] > 0:
            # There is alignment, so stay where we are
            return 1

        # insert a tab, but remove spaces first.
        if self.spaces:
            return focus.call("Replace", 1, ' '*self.spaces)

        m = mark.dup()
        len = 0
        while focus.call("doc:step", 0, 0, m, ret='char') == ' ':
            len += 1
            focus.call("doc:step", 0, 1, m)
        new = "\t" * (len / 8)
        focus.call("Replace", 1, m, mark, new)
        # fall through it insert a new tab
        return 0

    def handle_bs(self, key, focus, mark, **a):
        "handle:Backspace"
        # If in the indent, remove one level of indent
        m = mark.dup()
        c = focus.call("doc:step", 1, m, ret="char")
        if c and c in " \t":
            # Not at end of indent, fall through
            return 0
        c = focus.call("doc:step", 0, 0, m, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 0, 1, m)
            c = focus.call("doc:step", 0, 0, m, ret="char")
        if not (c is None or c == "\n"):
            # not at start of line, just fall through
            return 0
        if m == mark:
            # at start-of-line, fall-through
            return 0

        indent = self.calc_indent(focus, m)
        new = self.mkwhite(indent)
        current = focus.call("doc:get-str", m, mark, ret="str")
        # if current is more than expected, return to expected
        if current.startswith(new) and current != new:
            return focus.call("Replace", 1, m, mark, new)
        # if current is a prefix of expectation, reduce expection until not
        if new.startswith(current):
            while indent[0]:
                if indent[1]:
                    indent = (indent[0], 0, '')
                else:
                    indent[0].pop()
                new = self.mkwhite(indent)
                if current.startswith(new) and current != new:
                    return focus.call("Replace", 1, m, mark, new)
            return focus.call("Replace", 1, m, mark)
        # No clear relationship - replace wih new
        return focus.call("Replace", 1, m, mark, new)

    def handle_replace(self, key, focus, **a):
        "handle:Replace"
        if self.pre_paren:
            focus.call("doc:step", self.pre_paren[1], 1, 1)
            focus.call("Notify:change", self.pre_paren[0], self.pre_paren[1])
        self.pre_paren = None
        if self.post_paren:
            focus.call("doc:step", self.post_paren[1], 1, 1)
            focus.call("Notify:change", self.post_paren[0], self.post_paren[1])
        self.post_paren = None
        return 0

    def handle_refresh(self, key, focus, **a):
        "handle:Refresh"
        point = focus.call("doc:point", ret = 'mark')
        skip_pre = False
        skip_post = False
        if self.pre_paren:
            # maybe marks are still OK
            m = point.dup()
            if focus.call("doc:step", m, 0, 1, ret='char') and m == self.pre_paren[1]:
                skip_pre = True
            else:
                focus.call("doc:step", self.pre_paren[1], 1, 1)
                focus.call("Notify:change", self.pre_paren[0], self.pre_paren[1])
                self.pre_paren = None

        if self.post_paren:
            # maybe marks are still OK
            if point == self.post_paren[0]:
                skip_post = True
            else:
                focus.call("doc:step", self.post_paren[1], 1, 1)
                focus.call("Notify:change", self.post_paren[0], self.post_paren[1])
                self.post_paren = None

        if not skip_pre:
            c = focus.call("doc:step", point, 0, 0, ret = 'char')
            if c and c in ')}]':
                m2 = point.dup()
                focus.call("doc:step", m2, 0, 1)
                m1 = point.dup()
                focus.call("Move-Expr", m1, -1)
                c2 = focus.call("doc:step", m1, 1, 0, ret = 'char')
                if c2+c in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.pre_paren = (m1,m2)
                focus.call("Notify:change", m1, point)

        if not skip_post:
            c = focus.call("doc:step", point, 1, 0, ret = 'char')
            if c and c in '({[':
                m1 = point.dup()
                m2 = point.dup()
                focus.call("Move-Expr", m2, 1)
                focus.call("Notify:change", point, m2)
                focus.call("doc:step", m2, 0, 1)
                c2 = focus.call("doc:step", m2, 1, 0, ret = 'char')
                if c+c2 in "(){}[]":
                    m1['render:paren'] = "open"
                    m2['render:paren'] = "close"
                else:
                    m1['render:paren-mismatch'] = "open"
                    m2['render:paren-mismatch'] = "close"
                self.post_paren = (m1,m2)

        return 0

    def handle_map_attr(self, key, focus, mark, str, comm2, **a):
        "handle:map-attr"
        if str == "render:paren" and self.pre_paren and (mark in self.pre_paren):
            comm2("cb", focus, "bg:blue+50,bold", 1)
        if str == "render:paren" and self.post_paren and (mark in self.post_paren):
            comm2("cb", focus, "bg:blue+50,bold", 1)
        if str == "render:paren-mismatch" and self.pre_paren and (mark in self.pre_paren):
            comm2("cb", focus, "bg:red+50,bold", 1)
        if str == "render:paren-mismatch" and self.post_paren and (mark in self.post_paren):
            comm2("cb", focus, "bg:red+50,bold", 1)

    def handle_para(self, key, focus, mark, num, **a):
        "handle:Move-Paragraph"
        # A "Paragraph" is a function, which starts with a line that has no
        # indent, but does have a '('
        backward = 1
        if num > 0:
             backward = 0

        while num:
            try:
                focus.call("doc:step", mark, 1-backward, 1)
                l = focus.call("text-search", mark, "^([a-zA-Z0-9].*\(|\()",
                               0, backward)
                if not backward and l > 0:
                    while l > 1:
                        focus.call("doc:step", mark, 0, 1)
                        l -= 1
            except:
                break

            if num > 0:
                num -= 1
            else:
                num += 1
        focus.call("Move-to", mark)

        return 1

def c_mode_attach(key, focus, comm2, **a):
    p = CModePane(focus)
    p2 = p.call("attach-whitespace", ret='focus')
    if p2:
        p = p2
    comm2("callback", p)
    return 1

def py_mode_attach(key, focus, comm2, **a):
    p = CModePane(focus)
    p.spaces = 4
    p.indent_type = 'python'
    p2 = p.call("attach-whitespace", ret='focus')
    if p2:
        p = p2
    comm2("callback", p)
    return 1

def c_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-2:] in [".c", ".h"]:
        focus["view-default"] = "c-mode"
    return 0

def py_mode_appeared(key, focus, **a):
    n = focus["filename"]
    if n and n[-3:] in [".py"]:
        focus["view-default"] = "py-mode"
    return 0

def attach_indent(key, focus, **a):
    CModePane(focus)
    return 1

editor.call("global-set-command", "doc:appeared-c-mode", c_mode_appeared)
editor.call("global-set-command", "doc:appeared-py-mode", py_mode_appeared)
editor.call("global-set-command", "attach-c-mode", c_mode_attach)
editor.call("global-set-command", "attach-py-mode", py_mode_attach)
editor.call("global-set-command", "interactive-cmd-indent", attach_indent)
