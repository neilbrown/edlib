# -*- coding: utf-8 -*-
# Copyright Neil Brown (c)2018-2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

class CModePane(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.pre_paren = None
        self.post_paren = None
        self.spaces = None   # is set to a number, use spaces, else TABs
        self.indent_colon = False

    def handle_clone(self, key, focus, **a):
        "handle:Clone"
        p = CModePane(focus)
        p.spaces = self.spaces
        p.indent_colon = self.indent_colon
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

    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        # If there is white space before or after the cursor,
        # remove it.  Then work out how indented this line
        # should be, based on last non-empty line, and insert
        # that much space.
        m = mark.dup()
        c = focus.call("doc:step", 1, 0, m, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 1, 1, m)
            c = focus.call("doc:step", 1, 0, m, ret="char")
        focus.call("Move-to", m)
        c = focus.call("doc:step", 0, 1, m, ret="char")
        while c != None:
            if c not in " \t":
                focus.call("doc:step", 1, 1, m)
                break
            c = focus.call("doc:step", 0, 1, m, ret="char")
        indent_end = m.dup()
        new = self.find_indent(focus, indent_end)
        extra = self.find_extra_indent(focus, m, indent_end)

        return focus.call("Replace", 1, m, "\n" + self.tabify(new + extra))

    def handle_tab(self, key, focus, mark, **a):
        "handle:Tab"
        # if there is only white-space before cursor (up to newline) then:
        # move to end of white-space
        # - choose an indent as for Return
        # - If we don't have exactly that, replace with that
        # - if we do and chosen indent has no extra, add one indent level
        m = mark.dup()
        c = focus.call("doc:step", 0, 0, m, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 0, 1, m)
            c = focus.call("doc:step", 0, 0, m, ret="char")
        if not (c is None or c == "\n"):
            # not at start of line, just fall through
            return 0
        # m at start-of-line, move mark (point) to first non-white-space
        c = focus.call("doc:step", 1, 0, mark, ret="char")
        while c and c in " \t":
            focus.call("doc:step", 1, 1, mark)
            c = focus.call("doc:step", 1, 0, mark, ret="char")
        indent_end = m.dup()
        indent = self.find_indent(focus, indent_end)
        extra = self.find_extra_indent(focus, m, indent_end)
        current = focus.call("doc:get-str", m, mark, ret="str")
        new = self.tabify(indent + extra)
        if new != current:
            return focus.call("Replace", 1, m, mark, new)
        if extra == "":
            # round down to whole number of indents, and add 1
            if self.spaces:
                s = len(indent) % self.spaces
                extra = ' ' * (self.spaces - s)
            else:
                while indent and indent[-1] == ' ':
                    indent = indent[:-1]
                extra = '\t'
            return focus.call("Replace", 1, m, mark, self.tabify(indent + extra))
        # No change needed
        return 1

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
        indent_end = m.dup()
        indent = self.find_indent(focus, indent_end)
        extra = self.find_extra_indent(focus, m, indent_end)
        current = focus.call("doc:get-str", m, mark, ret="str")
        if extra:
            # if in the extra, delete back to 'indent'
            if len(current) > len(indent):
                return focus.call("Replace", 1, m, mark, indent)
        cnt = self.spaces
        if not cnt:
            cnt = 8
        m = mark.dup()
        c = focus.call("doc:step", 0, 0, m, ret="char")
        while c and c in " \t":
            if c == '\t' and cnt < 8:
                # don't go too far
                break
            focus.call("doc:step", 0, 1, m)
            if c == ' ':
                cnt -= 1
            else:
                cnt -= 8
            if cnt <= 0:
                break
            c = focus.call("doc:step", 0, 0, m, ret="char")
        return focus.call("Replace", 1, m, mark)

    def find_indent(self, focus, m):
        # Find previous line which is not empty and return
        # a string containing the leading tabs/spaces.
        # The mark is moved to the end of the indent.
        while focus.call("doc:step", 0, m, ret="char") == '\n':
            focus.call("doc:step", 0, 1, m)
        if focus.call("doc:step", 0, m) == edlib.WEOF:
            return ""
        # line before m is not empty
        m2 = m.dup()
        c = focus.call("doc:step", 0, m2, ret="char")
        while c != None and c != '\n':
            focus.call("doc:step", 0, 1, m2)
            if c not in " \t":
                m.to_mark(m2)
            c = focus.call("doc:step", 0, m2, ret="char")
        # m2 .. m is the prefix
        return focus.call("doc:get-str", m2, m, ret = 'str')

    def find_extra_indent(self, focus, m, indent_end):
        # now see if a () expression started since the indent.
        # If so, either line-up with open, or add a tab if open
        # is at end-of-line
        expr = m.dup()
        focus.call("Move-Expr", -1, 1, expr)
        if expr >= indent_end:
            focus.call("doc:step", expr, 1, 1)
            if focus.call("doc:step", expr, 1, 0, ret="char") == '\n':
                # open-bracket at end-of-line, so add a standard indent
                if self.spaces:
                    extra = ' ' * self.spaces
                else:
                    extra = "\t"
            else:
                extra = focus.call("doc:get-str", indent_end, expr, ret='str')
                extra = ' ' * len(extra)
        elif self.indent_colon:
            cl = m.dup()
            c = focus.call("doc:step", cl, 0, 1, ret="char")
            if c == '\n':
                c = focus.call("doc:step", cl, 0, 1, ret="char")
            if c == ':':
                if self.spaces:
                    extra = ' ' * self.spaces
                else:
                    extra = "\t"
            else:
                extra = ""
        else:
            extra = ""
        return extra

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
        focus.damaged(edlib.DAMAGED_CURSOR)
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
    p.indent_colon = True
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
