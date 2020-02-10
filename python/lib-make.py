# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016-2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import subprocess, os, fcntl

class MakePane(edlib.Pane):
    # This pane over-sees the running of "make" or "grep" command,
    # leaving output in a text document and handling make:match-docs
    # notifications sent to the document.
    #
    # Such notifications can cause the "next" match to be found and the
    # document containing it will be displayed in the 'focus' window,
    # with point at the target location.  A set 'view' of marks is used
    # to track the start of each line found.  Each mark has a 'ref'
    # which indexes into a local 'map' which records the filename and
    # line number.
    #
    # In each target file we place a mark at the line - so that edits
    # don't upset finding the next mark.
    # These are stored in a 'files' map which stores pane and mark 'view'.

    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.add_notify(focus, "make:match-docs")
        self.add_notify(focus, "make-close")
        self.add_notify(focus, "doc:make-revisit");
        self.viewnum = focus.call("doc:add-view", self) - 1
        self.point = None
        self.dir = self['dirname']
        self.map = []
        self.files = {}
        self.timer_set = False
        self.note_ok = False
        self.first_match = True
        self.call("editor:request:make:match-docs")

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
        self.call("doc:set:doc-status", "Running");
        self.call("doc:notify:doc:status-changed")
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
            (out, err) = self.pipe.communicate()
            self.pipe = None
            self.call("doc:replace", out.decode("utf-8"))
            self.call("doc:replace", "\nProcess Finished\n");
            self.call("doc:set:doc-status", "Complete")
            self.call("doc:notify:doc:status-changed")
            return edlib.Efalse
        self.call("doc:replace", r.decode("utf-8"));
        self.do_parse()
        return 1

    def do_parse(self):
        if self.timer_set:
            # wait for the timer
            return
        last = self.call("doc:vmark-get", self.viewnum, ret='mark2')
        if last:
            m = last.dup()
            self.call("doc:step", m, 1, 1)
        else:
            m = edlib.Mark(self)

        done = 0
        while done < 30:
            # Look for one of:
            # filename:linenum:.....
            # filename:linenum ....
            # ...FILE: filename:linenum:....
            # ...FILE: filename:linenum ....
            # FILE: is for checkpatch.
            # Entering directory '....'
            try:
                self.call("text-search",
                          "(^(.*FILE: )?[^: \t]+:[0-9]+[: ]|Entering directory ')",
                          m)
                last = self.call("doc:step", m, 0, 1, ret='char')
            except edlib.commandfailed:
                # No more matches - stop
                return
            if last == "'":
                # last char of match is ', so must be dir name
                self.call("doc:step", m, 1, 1)
                start = m.dup()
                rv = self.call("text-match", "[^'\n]*'", m)
                if rv > 0:
                    last = self.call("doc:step", m, 0, 1)
                    d = self.call("doc:get-str", start, m, ret='str')
                    if d:
                        if d[-1] == '/':
                            self.dir = d
                        else:
                            self.dir = d + '/'
                continue
            # Want to be careful of 'note: ' from gcc
            is_note = (self.call("text-match", ":[0-9]+: note:", m.dup()) > 0)

            # Now at end of line number.
            e = m.dup()
            while self.call("doc:step", m, 0, 1, ret='char') in "0123456789":
                pass
            s = m.dup()
            self.call("doc:step", s, 1, 1)
            lineno = self.call("doc:get-str", s, e, ret="str")
            e = m.dup()
            while self.call("doc:step", m, 0, 1, ret='char') not in [' ', '\n', None]:
                pass
            self.call("doc:step", m, 1, 1)
            fname = self.call("doc:get-str", m, e, ret="str")
            if self.first_match:
                self.call("doc:notify:make-set-match", m)
                self.first_match = False
            if self.record_line(fname, lineno, m, is_note):
                # new file - stop here
                break

            m.to_mark(e)
            done += 1
        # there are more matches - we aborted early.
        # set a timer
        if not self.timer_set:
            self.call("event:timer", 10, self.tick)
            self.timer_set = True
        return

    def tick(self, key, **a):
        self.timer_set = False
        self.do_parse()
        return edlib.Efalse

    def record_line(self, fname, lineno, m, is_note):
        d = edlib.Mark(self, self.viewnum)
        d.to_mark(m)
        d["ref"] = "%d" % len(self.map)
        prev = d.prev()
        if is_note and prev:
            prev['has_note'] = 'yes'
        self.map.append((fname, lineno))

        newfile = False
        if fname not in self.files:
            try:
                if fname[0] != '/':
                    dir = self.dir
                    if not dir:
                        dir = ""
                else:
                    dir = ""
                d = self.call("doc:open", -1, 8, dir+fname, ret='focus')
            except edlib.commandfailed:
                d = None
            if not d:
                return 0
            v = d.call("doc:add-view", self) - 1
            self.add_notify(d, "Notify:Close")
            self.files[fname] = (d, v)
            newfile = True
        (d,v) = self.files[fname]
        lm = edlib.Mark(d)
        ln = int(lineno)
        last = d.call("doc:vmark-get", v, self, ret='mark2')
        while last and int(last['line']) > ln:
            last = last.prev()
        if last:
            ln -= int(last['line']) - 1
            lm.to_mark(last)

        while lm and ln > 1:
            ch = d.call("doc:step", 1, 1, lm, ret='char')
            if ch == None:
                ln = 0
            elif ch == '\n':
                ln -= 1
        if ln == 1:
            mk = d.call("doc:vmark-get", self, v, lm, 3)
            if not mk or mk['line'] != lineno:
                mk = edlib.Mark(d, v, owner=self)
                mk.to_mark(lm)
                mk['line'] = lineno
        return newfile

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        for fn in self.files:
            (d,v) = self.files[fn]
            if d != focus:
                continue
            m = d.call("doc:vmark-get", self, v, ret='mark')
            while m:
                m.release()
                m = d.call("doc:vmark-get", self, v, ret='mark')
            d.call("doc:del-view", v, self)
            del self.files[fn]
            break

    def find_next(self):
        p = self.point
        if p:
            p["render:make-line"] = "no"
            self.call("doc:notify:doc:replaced", p, 100)
            t = p.prev()
            while t and t['has_note'] == 'yes':
                t['render:first_err'] = None
                self.call("doc:notify:doc:replaced", t, 100)
                t = t.prev()
        while True:
            if p:
                p = p.next()
            else:
                p = self.call("doc:vmark-get", self.viewnum, ret='mark')
            if not p:
                return None
            if p['has_note'] != 'yes' or self.note_ok:
                break
        self.point = p
        p["render:make-line"] = "yes"
        self.call("doc:notify:doc:replaced", p, 100)
        t = p.prev()
        while t and t['has_note'] == 'yes':
            t['render:first_err'] = 'yes'
            self.call("doc:notify:doc:replaced", t, 100)
            t = t.prev()
        self.note_ok = False
        return self.map[int(p['ref'])]

    def close_idle(self, cnt):
        if self['cmd'] == 'make':
            # don't close 'make'
            return
        if self.pipe:
            # command is still running
            return
        if self.timer_set:
            # still busy
            return
        p = self.point
        while p:
            p = p.next()
            if p and p['has_note'] != 'yes':
                break
        if p and cnt < 5:
            # Haven't visited the last match yet, and this is
            # a relatively recent 'grep' - might still be interesting.
            return 0
        if not self.parent.notify("doc:notify-viewers"):
            self.parent.close()
            return 0

    def make_next(self, key, focus, num, str, str2, xy, **a):
        "handle:make:match-docs"
        if str == "close-idle":
            return self.close_idle(xy[0])
        if str != "next-match":
            return 0
        prevret = xy[1]
        if prevret > 0:
            # some other pane has already responded
            return 0

        if num:
            # clear marks so that we parse again
            m = self.call("doc:vmark-get", self.viewnum, ret='mark')
            while m:
                m.release()
                m = self.call("doc:vmark-get", self.viewnum, ret='mark')
            self.point = None
            self.dir = self['dirname']
            self.note_ok = False

        self.do_parse()
        n = self.find_next()
        if not n:
            focus.call("Message", "No further matches")
            # send viewers to keep following end of file.
            self.first_match = True
            if self.point:
                self.call("doc:set-ref", self.point)
                self.call("doc:notify:make-set-match", self.point)
            return 0
        if not str2:
            str2 = "ThisPane"
        try:
            self.goto_mark(focus, n, str2)
        except edlib.commandfailed:
            # error already reported
            pass
        return 1

    def goto_mark(self, focus, n, where):
        (fname, lineno) = n
        if fname in self.files:
            (d,v) = self.files[fname]
            mk = d.call("doc:vmark-get", self, v, ret='mark')
            while mk and int(mk['line']) < int(lineno):
                mk = mk.next()
            if mk and int(mk['line']) == int(lineno):
                self.visit(focus, d, mk, lineno, where)
                return 1
            self.visit(focus, d, None, lineno, where)
            return 1
        # try the old way
        try:
            if fname[0] != '/':
                dir = self.dir
                if not dir:
                    dir = ""
            else:
                dir = ""
            # 8 means reload
            d = focus.call("doc:open", -1, 8, dir+fname, ret='focus')
        except edlib.commandfailed:
            d = None
        if not d:
            if dir:
                focus.call("Message", "File %s not found in %s." %( fname, dir))
            else:
                focus.call("Message", "File %s not found." % fname)
            return edlib.Efail
        self.visit(focus, d, None, lineno, where)
        return 1

    def visit(self, focus, d, mk, lineno, where):
        par = focus.call("DocPane", d, ret='focus')
        if par:
            while par.focus:
                par = par.focus
        else:
            par = focus.call(where, d, ret='focus')
            if not par:
                d.close()
                focus.call("Message", "Failed to open pane");
                return edlib.Efail
            par = d.call("doc:attach-view", par, 1, ret='focus')
        par.take_focus()
        if mk and (int(lineno) == 1 or
                   d.call("doc:step", mk, 0, ret='char') != None):
            par.call("Move-to", mk)
        else:
            # either no mark, or the mark has moved to start of doc, probably
            # due to a reload
            par.call("Move-File", -1)
            par.call("Move-Line", int(lineno)-1)

        docpane = par.call("DocPane", self, ret='focus')
        if not docpane:
            docpane = par.call("OtherPane", ret='focus')
            if docpane:
                self.call("doc:attach-view", docpane)
        self.call("doc:notify:make-set-match", self.point)
        return 1

    def handle_revisit(self, key, mark, **a):
        "handle:doc:make-revisit"
        self.do_parse()
        p = self.call("doc:vmark-get", self.viewnum, mark, 3, ret='mark2')
        if self.point:
            self.point["render:make-line"] = "no"
            self.call("doc:notify:doc:replaced", self.point, 100)
            if p:
                t = p.prev()
            else:
                t = None
            while t and t['has_note'] == 'yes':
                t['render:first_err'] = None
                self.call("doc:notify:doc:replaced", t, 100)
                t = t.prev()
        if p:
            self.point = p.prev()
            self.note_ok = True
        # move to front of match-docs list
        self.drop_notify("make:match-docs");
        self.call("editor:request:make:match-docs")
        return 1

    def handle_make_close(self, key, **a):
        "handle:make-close"
        # make output doc is being reused, so we'd better get out of the way
        self.close()
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

        for fn in self.files:
            (d,v) = self.files[fn]
            m = d.call("doc:vmark-get", self, v, ret='mark')
            while m:
                m.release()
                m = d.call("doc:vmark-get", self, v, ret='mark')
            d.call("doc:del-view", v, self)
        del self.files

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
    # delete current content
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
    # This is a simple overlay to allow :Enter to
    # jump to a given match, and similar
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.call("doc:request:doc:replaced")
        self.call("doc:request:make-set-match")

    def handle_set_match(self, key, mark, **a):
        "handle:make-set-match"
        self.call("Move-to", mark)
        return 1

    def handle_enter(self, key, focus, mark, **a):
        "handle:K:Enter"
        focus.call("doc:notify:doc:make-revisit", mark)
        next_match("interactive-cmd-next-match", focus,
                   edlib.NO_NUMERIC, "OtherPane")
        return 1

    def handle_kill(self, key, **a):
        "handle:K-q"
        self.call("doc:destroy")
        return 1

    def handle_replace(self, key, mark, mark2, **a):
        "handle:doc:replaced"
        if not mark or not mark2:
            return 1
        p = self.call("doc:point", ret='mark')
        if p and p == mark:
            # point is where we inserted text, so move it to
            # after the insertion point
            p.to_mark(mark2)
        return 1

    def handle_clone(self, key, focus, home, **a):
        "handle:Clone"
        p = MakeViewerPane(focus)
        home.clone_children(p)

    def handle_highlight(self, key, focus, str, str2, mark, comm2, **a):
        "handle:map-attr"
        if not comm2:
            return
        if str == "render:make-line" and str2 == "yes":
            comm2("attr:callback", focus, mark, "bg:cyan+80", 10000, 2)
            return 1
        if str == "render:first_err" and str2 == "yes":
            comm2("attr:callback", focus, mark, "bg:magenta+80", 10000, 2)
            return 1

def make_view_attach(key, focus, comm2, **a):
    p = focus.call("attach-viewer", ret='focus')
    p = MakeViewerPane(p)

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
        "handle:K:Enter"
        str = focus.call("doc:get-str", ret="str")
        return focus.call("popup:close", str)

def isword(c):
    return c and c.isalnum() or c == '_'

def run_make(key, focus, str, num, **a):
    # pop-up has completed
    c = key.index(':')
    dir = key[c+1:]
    mode = key[:c]
    if mode == "git":
        cmd = "git-grep"
        docname = "*Grep Output*"
    elif mode == "grep" or mode == "TAGS" or mode == "quilt":
        cmd = "grep"
        docname = "*Grep Output*"
    else:
        cmd = "make"
        docname = "*Compile Output*"
    # Destroying the document might destroy my focus, so
    # grab a reference to the tile
    tile = focus.call("ThisPane", ret='focus')
    if tile:
        focus = tile
    doc = None
    if cmd == "make":
        # try to reuse old document
        try:
            doc = focus.call("docs:byname", docname, ret='focus')
            if num == 42:
                # use directory from previous run
                dir = doc['dirname']
            doc.call("doc:clear")
            doc.notify("make-close")
        except edlib.commandfailed:
            pass
    else:
        # Tell any extend grep documents that
        # a/ are no longer running, b/ have point at the end, c/ are not
        # visible; to close
        focus.call("editor:notify:make:match-docs", "close-idle");
    if not doc:
        # If doing 'grep', or if there is no make output doc, create new one.
        doc = focus.call("doc:from-text", docname, "", ret='focus')
        if not doc:
            return edlib.Efail

    doc['dirname'] = dir
    doc['view-default'] = 'make-viewer'
    if cmd == "make":
        p = focus.call("DocPane", doc, ret='focus')
        if not p:
            p = focus.call("OtherPane", doc, ret='focus')
    else:
        p = focus.call("PopupTile", "MD3ta", ret='focus')
    if not p:
        return edlib.Efail
    p = doc.call("doc:attach-view", p, 1, ret='focus')

    p = doc.call("attach-makecmd", str, dir, ret='focus')
    p['cmd'] = cmd
    return 1

def make_request(key, focus, num, str, mark, **a):
    history = None
    dflt_arg = ''
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
            # Make 'num' and 'str' available after save-all
            if str:
                p['default'] = "%d,%s" % (num, str)
            else:
                p['default'] = "%d" % (num)
            p.call("docs:show-modified")
            return 1
        if num == 1 and not (str is None):
            # We did a save-all, restore num and str
            s = str.split(',',1)
            str = None
            if len(s) > 1:
                str = s[1]
            try:
                num = int(s[0])
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
            if os.path.exists(os.path.join(d, ".git")):
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

        if num > 0 and mode != "grep":
            # if we found a project-root, run command from there.
            if d and d[-1] != '/':
                d = d + '/'
            dir = d

    if cmd != "make" and mark and focus['doc-type'] == "text":
        # choose the word under the cursor
        if not str:
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
        if str and not ('\n' in str):
            if not "'" in str:
                dflt_arg = "'" + str + "'"
            elif not '"' in str:
                dflt_arg = '"' + str + '"'
            else:
                dflt_arg = str

    if cmd == "make" and num:
        # re-use previous command
        make_cmd = focus.call("history-get-last", history, ret='str')
        if make_cmd:
            run_make("%s:%s"%(mode,dir), focus, make_cmd, 42)
            return 1

    # Create a popup to ask for make command
    p = focus.call("PopupTile", "D2", dflt, ret="focus")
    if not p:
        return 0
    if dflt_arg:
        p.call("mode-set-mark")
        p.call("Replace", dflt_arg)
    p.call("popup:set-callback", run_make)
    p["prompt"] = "%s Command" % cmd
    p["done-key"] = "%s:%s" % (mode, dir)
    p.call("doc:set-name", "%s Command" % cmd)
    if history:
        p = p.call("attach-history", history, "popup:close", ret='focus')
    if dir:
        p["dirname"] = dir
    makeprompt(p)
    return 1

def next_match(key, focus, num, str, **a):
    if num == edlib.NO_NUMERIC:
        restart = 0
    else:
        restart = 1
    if not focus.call("editor:notify:make:match-docs", "next-match",
                      str, restart):
        focus.call("Message", "No next-match found")

    return 1

editor.call("global-set-command", "attach-makecmd", make_attach)
editor.call("global-set-command", "attach-make-viewer", make_view_attach)
editor.call("global-set-command", "interactive-cmd-make", make_request)
editor.call("global-set-command", "interactive-cmd-grep", make_request)
editor.call("global-set-command", "interactive-cmd-git-grep", make_request)
editor.call("global-set-command", "interactive-cmd-next-match", next_match)
