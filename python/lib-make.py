# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import subprocess, os, fcntl

class MakePane(edlib.Pane):
    # This pane over-sees the running of "make" or "grep" command,
    # leaving output in a text document and handling make-next
    # notifications sent to the document.
    # Such notifications cause the "next" match to be found
    # and the document containing it will be displayed
    # in the 'focus' window, with point at the target location.
    # A set 'view' of marks is used to track the start of each line
    # found.  Each mark has a 'ref' which indexes into a local 'map'
    # which records the filename and line number.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.add_notify(focus, "make-next")
        self.add_notify(focus, "Notify:doc:make-revisit");
        self.viewnum = focus.call("doc:add-view", self) - 1
        self.point = None
        self.map = []

    def run(self, cmd, cwd):
        FNULL = open(os.devnull, 'r')
        self.call("doc:replace", "Cmd: %s\nCwd: %s\n\n" % (cmd,cwd))
        self.pipe = subprocess.Popen(cmd, shell=True, close_fds=True,
                                     cwd=cwd,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     stdin = FNULL)
        FNULL.close()
        if not self.pipe:
            return False
        self.call("doc:set:doc:status", "Running");
        self.notify("doc:status-changed")
        fd = self.pipe.stdout.fileno()
        fl = fcntl.fcntl(fd, fcntl.F_GETFL)
        fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
        self.call("event:read", fd, self.read)
        return True

    def read(self, key, **a):
        if not self.pipe:
            return edlib.Efalse
        try:
            r = os.read(self.pipe.stdout.fileno(), 1024)
        except IOError:
            return 1
        if r is None or len(r) == 0:
            self.pipe.communicate()
            self.pipe = None
            self.call("doc:replace", "\nProcess Finished\n");
            self.call("doc:set:doc:status", "Complete")
            self.notify("doc:status-changed")
            return edlib.Efalse
        self.call("doc:replace", r);
        return 1

    def do_parse(self):
        last = self.call("doc:vmark-get", self.viewnum, ret='mark2')
        if last:
            m = last.dup()
            self.call("doc:step", m, 1, 1)
        else:
            m = edlib.Mark(self)

        while True:
            try:
                self.call("text-search", "^[^: \t]+:[0-9]+[: ]", m)
            except edlib.commandfailed:
                break
            self.call("doc:step", m, 0, 1)
            e = m.dup()
            while self.call("doc:step", m, 0, 1, ret='char') in "0123456789":
                pass
            s = m.dup()
            self.call("doc:step", s, 1, 1)
            lineno = self.call("doc:get-str", s, e, ret="str")
            e = m.dup()
            while self.call("doc:step", m, 0, 1, ret='char') not in ['\n', None]:
                pass
            self.call("doc:step", m, 1, 1)
            fname = self.call("doc:get-str", m, e, ret="str")
            d = edlib.Mark(self, self.viewnum)
            d.to_mark(m)
            d["ref"] = "%d" % len(self.map)
            self.map.append((fname, lineno))

            m.to_mark(e)

    def find_next(self):
        p = self.point
        if p:
            p = p.next()
        else:
            p = self.call("doc:vmark-get", self.viewnum, ret='mark')
        if not p:
            return None
        self.point = p
        return self.map[int(p['ref'])]

    def make_next(self, key, focus, **a):
        "handle:make-next"
        self.do_parse()
        n = self.find_next()
        if not n:
            focus.call("Message", "No further matches")
            return 1
        return self.goto_mark(focus, n, "ThisPane")

    def goto_mark(self, focus, n, where):
        (fname, lineno) = n
        try:
            dir = self['dirname']
            if not dir:
                dir = ""
            d = focus.call("doc:open", -1, dir+fname, ret='focus')
        except edlib.commandfailed:
            d = None
        if not d:
            focus.call("Message", "File %s not found in %s." %( fname, dir))
            return edlib.Efail
        par = focus.call("DocPane", d, ret='focus')
        if not par:
            par = focus.call(where, d, ret='focus')
            if not par:
                d.close()
                focus.call("Message", "Failed to open pane");
                return edlib.Efail
            par = d.call("doc:attach-view", par, ret='focus')
        par.take_focus()
        par.call("Move-File", -1)
        par.call("Move-Line", int(lineno)-1)

        par = focus.call("DocPane", self, ret='focus')
        if par:
            while par.focus:
                par = par.focus
            par.call("Move-to", self.point)
        return 1

    def handle_revisit(self, key, focus, mark, **a):
        "handle:Notify:doc:make-revisit"
        self.do_parse()
        p = self.call("doc:vmark-get", self.viewnum, mark, 3, ret='mark2')
        if p:
            self.point = p
            n = self.map[int(p['ref'])]
            if n:
                return self.goto_mark(focus, n, "OtherPane")
        return 1

    def handle_close(self, key, **a):
        "handle:Close"
        if self.pipe is not None:
            p = self.pipe
            self.pipe = None
            p.terminate()
            try:
                p.communicate()
            except IOError:
                pass
        m = self.call("doc:vmark-get", self.viewnum, ret='mark')
        while m:
            m.release()
            m = self.call("doc:vmark-get", self.viewnum, ret='mark')
        self.call("doc:del-view", self.viewnum)
        return 1

    def handle_abort(self, key, **a):
        "handle:Abort"
        if self.pipe is not None:
            self.pipe.terminate()
            self.pipe.communicate()
            self.pipe = None
            self.call("doc:replace", "\nProcess Aborted\n");
        return 1

def make_attach(key, focus, comm2, str, str2, **a):
    m = edlib.Mark(focus)
    focus.call("doc:replace", m)
    p = MakePane(focus)
    if not p:
        return edlib.Efail
    if not p.run(str, str2):
        p.close()
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

class MakeViewerPane(edlib.Pane):
    # This is a simple overlay to allow Enter to
    # jump to a given match, and similar
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_enter(self, key, focus, mark, **a):
        "handle:Enter"
        focus.call("doc:Notify:doc:make-revisit", mark)
        return 1

    def handle_clone(self, key, focus, home, **a):
        "handle:Clone"
        p = MakeViewerPane(focus)
        home.clone_children(p)

def make_view_attach(key, focus, comm2, **a):
    p = MakeViewerPane(focus)

    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 0

class makeprompt(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.call("attach-file-entry", "shellcmd")

    def enter(self, key, focus, **a):
        "handle:Enter"
        str = focus.call("doc:get-str", ret="str")
        return focus.call("popup:close", str)

def isword(c):
    return c and c.isalnum() or c == '_'

def run_make(key, focus, str, **a):
    # pop-up has completed
    c = key.index(':')
    dir = key[c+1:]
    mode = key[:c-1]
    if mode == "git":
        cmd = "git-grep"
        docname = "*Grep Output*"
    elif mode == "grep" or mode == "TAGS" or mode == "quilt":
        cmd = "grep"
        docname = "*Grep Output*"
    else:
        cmd = "make"
        docname = "*Compile Output*"
    try:
        doc = focus.call("docs:byname", docname, ret='focus')
        doc.call("doc:destroy")
    except edlib.commandfailed:
        pass
    doc = focus.call("doc:from-text", docname, "", ret='focus')
    if not doc:
        return edlib.Efail

    doc['dirname'] = dir
    if cmd == "make":
        p = focus.call("OtherPane", ret='focus')
    else:
        p = focus.call("PopupTile", "MD3t", ret='focus')
    if not p:
        return edlib.Efail
    focus.call("global-set-attr", "make-target-doc", docname)
    p = doc.call("doc:attach-view", p, "viewer", ret='focus')
    p.call("attach-make-viewer", p);

    p = doc.call("attach-makecmd", str, dir, ret='focus')
    return 1
    

def make_request(key, focus, num, str, mark, **a):
    history = None
    if key[-8:] == "git-grep":
        dflt = "grep -rnH "
        cmd = "git-grep"
        history = "*Git-grep History*"
    elif key[-4:] == "grep":
        dflt = "grep -nH "
        cmd = "grep"
        history = "*Grep History*"
    else:
        if focus.call("docs:save-all", 0, 1) != 1:
            p = focus.call("PopupTile", "DM", ret='focus');
            p['done-key'] = key
            # Make 'num' available after save-ll
            p['default'] = "%d" % num
            p.call("docs:show-modified")
            return 1
        if num == 1 and not (str is None):
            # We did a save-all, restore num
            try:
                num = int(str)
            except:
                num = 0
        dflt = "make -k"
        cmd = "make"
        history = "*Make History*"
    mode = cmd

    dir = focus['dirname']
    if cmd == "git-grep":
        # Walk up tree looking for .git or .pc or TAGS
        # depending on what is found, choose an approach
        d = dir
        mode = "grep"
        while d and d != '/' and mode == "grep":
            if os.path.isdir(os.path.join(d, ".git")):
                mode = "git"
                dflt = "git grep -nH "
            elif os.path.isfile(os.path.join(d, "TAGS")):
                mode = "TAGS"
                dflt = "grep -rnH "
            elif os.path.isdir(os.path.join(d, ".pc")):
                mode = "quilt"
                dflt = "grep -rnH --exclude-dir=.pc "
            else:
                d = os.path.dirname(d)

        if num and num > 0 and mode != "grep":
            dir = d + '/'

    if cmd != "make" and num and mark and focus['doc-type'] == "text":
        # choose the word under the cursor
        m1 = mark.dup()
        c = focus.call("doc:step", m1, ret='char')
        while isword(c):
            focus.call("doc:step", m1, 0, 1)
            c = focus.call("doc:step", m1, ret='char')
        m2 = mark.dup()
        c = focus.call("doc:step", m2, 1, ret='char')
        while isword(c):
            focus.call("doc:step", m2, 1, 1)
            c = focus.call("doc:step", m2, 1, ret='char')
        str = focus.call("doc:get-str", m1, m2, ret='str')
        if not ('\n' in str):
            dflt = dflt + str

    if cmd == "make" and num:
        # re-use previous command
        make_cmd = focus.call("history-get-last", history, ret='str')
        if make_cmd:
            run_make("%s:%s"%(mode,dir), focus, make_cmd)
            return 1

    # Create a popup to ask for make command
    p = focus.call("PopupTile", "D2", dflt, ret="focus")
    if not p:
        return 0
    p.call("popup:set-callback", run_make)
    p["prompt"] = "%s Command" % cmd
    p["done-key"] = "%s:%s" % (mode, dir)
    p.call("doc:set-name", "%s Command" % cmd)
    if history:
        p = p.call("attach-history", history, "popup:close", ret='focus')
    dn = focus["dirname"]
    if dn:
        p["dirname"] = dn
    makeprompt(p)
    return 1

def next_match(key, focus, **a):
    docname = focus["make-target-doc"]
    if not docname:
        focus.call("Message", "No make output!")
        return 1
    try:
        doc = focus.call("docs:byname", docname, ret='focus')
    except edlib.commandfailed:
        doc = None
    if not doc:
        focus.call("Message", "Make document %s missing" % docname)
        return 1
    try:
        doc.notify("make-next", focus)
    except edlib.commandfailed:
        pass
    return 1

editor.call("global-set-command", "attach-makecmd", make_attach)
editor.call("global-set-command", "attach-make-viewer", make_view_attach)
editor.call("global-set-command", "interactive-cmd-make", make_request)
editor.call("global-set-command", "interactive-cmd-grep", make_request)
editor.call("global-set-command", "interactive-cmd-git-grep", make_request)
editor.call("global-set-command", "interactive-cmd-next-match", next_match)
