# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# perform calculations on lines starting '?', placing results in lines
# starting '>'
#

class CalcView(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.vars = {}
        self.varcnt = 0
        self.getvar = focus.call("make-search", "> ([a-z0-9]+) = ", 3, ret='comm')

    def handle_enter(self, key, focus, mark, **a):
        "handle-list/K:Enter/K:M:Enter"
        if not mark:
            return edlib.Enoarg
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        c = focus.call("doc:step", 1, m, ret='char')
        if c == '?':
            if key == "K:M:Enter":
                while self.calc(focus,mark):
                    pass
            else:
                self.calc(focus, mark)
            return 1
        if c == '>':
            return self.add_expr(focus, mark)
        return 0

    def nextvar(self):
        # from 0 to 675 use 2 letters
        # beyond that use 3.
        nm = None
        while not nm:
            v = self.varcnt
            self.varcnt += 1
            nm = chr(97+v%26)
            v = int(v/26)
            nm = chr(97+v%26) + nm
            v = int(v/26)
            while v:
                nm = chr(97+v%26) + nm
                v = int(v/26)
            if nm in self.vars:
                # In use, try next one
                nm = None
        return nm

    def calc(self, focus, mark):
        m = mark.dup()
        focus.call("Move-EOL", -1, m)
        focus.call("Move-EOL", 1, mark)
        s = focus.call("doc:get-str", m, mark, ret='str')
        if s[0] == '?':
            s = s[1:]
        self.result = ""; self.hex = None; self.float = None
        self.err = 0
        formats = 'xf'
        if s and s[-1] == '@':
            formats = 'of'
            s = s[:-1]
        if not s.strip():
            # Empty expression, son't bother
            return False
        try:
            focus.call("CalcExpr", s, formats, self.take_result)
        except edlib.commandfailed:
            pass
        if self.result:
            if self.hex:
                a = self.result + " " + self.hex
            elif self.float:
                a = self.float + " " + self.result
            else:
                a = self.result

            m = mark.dup()
            c = focus.call("doc:step", 1, 1, m, ret='char')
            nm = None
            if c and c == '\n':
                c = focus.call("doc:step", 1, m, ret='char')
                if c and c == '>':
                    self.getvar("reinit", focus, 3)
                    focus.call("doc:content", m.dup(), self.getvar)
                    nm = self.getvar("interp", focus, "\\1", ret='str')
                    # replace this line
                    focus.call("Move-EOL", 1, m)
                else:
                    m = mark.dup()
            else:
                m = mark.dup()
            if not nm:
                nm = self.nextvar()
            self.vars[nm] = self.result
            focus.call("doc:replace", "\n> %s = %s" %(nm, a), mark, m)
            mark.to_mark(m)
            c = focus.call("doc:step", 1, 1, mark, ret='char')
            if not c:
                focus.call("doc:replace", "\n? ", mark, mark)
            else:
                c = focus.call("doc:step", 1, mark, ret='char')
                if c and c == '?':
                    focus.call("Move-EOL", 1, mark)
                    # keep going
                    return True
                else:
                    focus.call("doc:replace", "? \n", mark, mark)
                    focus.call("Move-Char", -1, mark)
        else:
            focus.call("Message:modal", "Calc failed for %s: %s" %(s,self.err))
        return False

    def take_result(self, key, focus, num, str, comm2, **a):
        if key == "get":
            if str in self.vars and comm2:
                comm2("cb", focus, self.vars[str])
                return 1
            return edlib.Efalse
        edlib.LOG(key, str)
        if key == "result":
            self.result = str
        if key == "hex-result" or key == "oct-result":
            self.hex = str
        if key == "frac-result":
            self.result = str
        if key == "float-result":
            self.float = str
        if key == "err":
            self.err = num
        return 1

    def add_expr(self, focus, mark):
        # add new expression line after this line
        focus.call("Move-EOL", 1, mark)
        c = focus.call("doc:step", 1, 1, mark, ret='char')
        if not c:
            # No EOL char
            focus.call("doc:replace", "\n? \n", mark.dup(), mark)
        else:
            focus.call("doc:replace", "? \n", mark.dup(), mark)
        focus.call("doc:step", 0, 1, mark)
        return 1

def calc_view_attach(key, focus, comm2, **a):
    p = CalcView(focus)
    if not p:
        return edlib.Efail
    if comm2:
        comm2("cb", p)
    return 1

def add_calc(key, focus, mark, **a):
    p = CalcView(focus)
    if p:
        p.call("view:changed")

    v = focus['view-default']
    if v:
        v = v + ',view-calc'
    else:
        v = 'view-calc'
    focus.call("doc:set:view-default", v)
    return 1

editor.call("global-set-command", "attach-view-calc", calc_view_attach)
editor.call("global-load-module", "lib-calc")
editor.call("global-set-command", "interactive-cmd-calc", add_calc)
