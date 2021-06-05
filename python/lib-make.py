# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2016-2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import os, fcntl, signal

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
        self.doc = focus.parent
        self.call("doc:request:make:match-docs")
        self.call("doc:request:make-close")
        self.call("doc:request:doc:make-revisit")
        self.call("doc:request:doc:replaced")
        self.viewnum = focus.call("doc:add-view", self) - 1
        self.point = None
        self.dirs = {self['dirname']: 100}
        self.map = []
        self.files = {}
        self.timer_set = False
        self.note_ok = False
        self.first_match = True
        self.backwards = False
        self.call("editor:request:make:match-docs")
        self.last = None # last (file,line) seen
        self.pos = None # mark where we have parsed up to

    def replaced(self, key, **a):
        "handle:doc:replaced"
        self.do_parse()
        return 1

    def do_parse(self):
        if self.timer_set:
            # wait for the timer
            return
        last = self.pos
        if last:
            m = last.dup()
            self.next(m)
        else:
            m = edlib.Mark(self)

        done = 0
        self.set_time()
        while done < 100 and not self.too_long():
            # Look for one of:
            # filename:linenum:.....
            # filename:linenum ....
            # ...FILE: filename:linenum:....
            # ...FILE: filename:linenum ....
            # FILE: is for checkpatch.
            # Entering directory '....'
            try:
                self.call("text-search",
                          "(^(.*FILE: )?[^: \t]+:[0-9]+[: ]|(Entering|Leaving) directory ')",
                          m)
                last = self.prev(m)
            except edlib.commandfailed:
                # No more matches - stop
                return
            if last == "'":
                # last char of match is ', so must be dir name
                self.next(m)
                start = m.dup()
                while self.prev(m) not in 'EL':
                    pass
                # get "Entering directory" or "Leaving Directory"
                el = self.call("doc:get-str", m ,start, ret='str')
                m.to_mark(start)
                rv = self.call("text-match", "[^'\n]*'", m)
                if rv > 0:
                    last = self.prev(m)
                    d = self.call("doc:get-str", start, m, ret='str')
                    if d:
                        if d[-1] == '/':
                            dir = d
                        else:
                            dir = d + '/'
                        if el.startswith('Entering'):
                            try:
                                self.dirs[dir] += 1
                            except KeyError:
                                self.dirs[dir] = 1
                        else:
                            # must be Leaving
                            try:
                                self.dirs[dir] -= 1
                                if self.dirs[dir] <= 0:
                                    del self.dirs[dir]
                            except KeyError:
                                pass
                self.pos = m.dup()
                continue
            # Want to be careful of 'note: ' from gcc
            is_note = (self.call("text-match", ":[0-9]+: note:", m.dup()) > 0)

            # Now at end of line number.
            e = m.dup()
            while self.prev(m) in "0123456789":
                pass
            s = m.dup()
            self.next(s)
            lineno = self.call("doc:get-str", s, e, ret="str")
            e = m.dup()
            while self.prev(m) not in [' ', '\n', None]:
                pass
            self.next(m)
            fname = self.call("doc:get-str", m, e, ret="str")
            self.pos = e.dup()
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
            dir = ""
            if fname[0] != '/':
                for d in self.dirs:
                    if os.path.isfile(os.path.join(d, fname)):
                        dir = d
                        break
            try:
                d = self.call("doc:open", -1, 8, dir+fname, ret='focus')
            except edlib.commandfailed:
                d = None
            if not d:
                return False
            v = d.call("doc:add-view", self) - 1
            self.add_notify(d, "Notify:Close")
            self.files[fname] = (d, v)
            newfile = True
        (d,v) = self.files[fname]
        lm = edlib.Mark(d)
        ln = int(lineno)
        first, last = d.vmarks(v, self)
        while last and int(last['line']) > ln:
            last = last.prev()
        if last:
            ln -= int(last['line']) - 1
            lm.to_mark(last)

        while lm and ln > 1:
            ch = d.next(lm)
            if ch == None:
                ln = 0
            elif ch == '\n':
                ln -= 1
        if ln == 1:
            mk = d.vmark_at_or_before(v, lm, self)
            if not mk or mk['line'] != lineno:
                mk = edlib.Mark(d, v, owner=self)
                mk.to_mark(lm)
                mk['line'] = lineno
        return newfile

    def handle_notify_close(self, key, focus, **a):
        "handle:Notify:Close"
        for fn in self.files:
            (d,v) = self.files[fn]
            if d == focus:
                del self.files[fn]
                break

    def clear_render(self):
        if not self.point:
            return
        p = self.point
        # clear out render: markings for current set.
        while p.prev() and p.prev()['has_note']:
            p = p.prev()
        # now at the start of this match
        while p and p['has_note']:
            p['render:make-line'] = None
            self.call("doc:notify:doc:replaced", p, 100)
            p = p.next()
        if p:
            # no has_note, so last in list
            p['render:make-line'] = None
            self.call("doc:notify:doc:replaced", p, 100)
            p = p.next()
        return p

    def find_next(self):
        if self.note_ok:
            # stepping through notes rather than full matches
            if self.point:
                p = self.point
                p['render:make-line'] = None
                self.call("doc:notify:doc:replaced", p, 100)
                if p['has_note'] or self.note_ok == "First":
                    p = p.next()
                    self.note_ok = True
                else:
                    p = None
            else:
                p, l = self.vmarks(self.viewnum)
            if p:
                # p is the next note to show
                self.point = p
                p['render:make-line'] = 'best'
                self.call("doc:notify:doc:replaced", p, 100)
                return self.map[int(p['ref'])]
        self.note_ok = False
        if self.point:
            p = self.clear_render()
        else:
            p, l = self.vmarks(self.viewnum)
        if not p:
            return None
        self.point = None
        while p and p['has_note']:
            p['render:make-line'] = 'other'
            if not self.point:
                (fname, lineno) = self.map[int(p['ref'])]
                if fname[-2:] == '.c':
                    self.point = p
            self.call("doc:notify:doc:replaced", p, 100)
            p = p.next()
        if not p:
            # Not possible as previous had a note
            return None
        p['render:make-line'] = 'other'
        self.call("doc:notify:doc:replaced", p, 100)
        if not self.point:
            self.point = p
        self.point['render:make-line'] = 'best'
        self.call("doc:notify:doc:replaced", self.point, 100)
        return self.map[int(self.point['ref'])]

    def find_prev(self):
        if not self.point:
            return None
        p = self.point.prev()
        self.clear_render()
        if not p:
            return None
        p['render:make-line'] = 'best'
        self.call("doc:notify:doc:replaced", p, 100)
        self.point = p
        return self.map[int(self.point['ref'])]

    def close_idle(self, cnt):
        if self['cmd'] == 'make':
            # don't close 'make'
            return
        if self['doc-status'] == 'Running':
            # command is still running
            return
        if self.timer_set:
            # still busy
            return
        p = self.point
        while p:
            p = p.next()
            if p and not p['has_note']:
                break
        if p and cnt < 5:
            # Haven't visited the last match yet, and this is
            # a relatively recent 'grep' - might still be interesting.
            return 0
        if not self.doc.notify("doc:notify-viewers"):
            self.doc.close()
            return 0

    def make_next(self, key, focus, num, num2, str, str2, xy, comm2, **a):
        "handle:make:match-docs"
        prevret = xy[1]
        if str == "close-idle":
            return self.close_idle(xy[0])
        if str == "choose-make":
            if self['cmd'] != 'make':
                return 0
            if prevret > 0:
                return 0
            if (str2.startswith(self['dirname']) or
                str2.startswith(self['realdir'])):
                # This is a suitable make document for the given directory
                if comm2:
                    comm2("cb", self.doc)
                return 1
            return 0

        if prevret > 0:
            # some other pane has already responded
            return 0

        if str == "find-visible":
            # If this doc is visible from 'focus', return the pane
            # it is visible in.
            docpane = focus.call("DocPane", self.doc, ret='focus')
            if not docpane:
                return 0
            if comm2:
                comm2("cb", docpane)
            return 1

        if str != "next-match":
            return 0

        if num < 0:
            self.backwards = True
        elif num > 0 and self.backwards:
            self.backwards = False
        elif num > 0:
            # clear marks so that we parse again
            m, l = self.vmarks(self.viewnum)
            while m:
                m.release()
                m, l = self.vmarks(self.viewnum)
            self.map = []
            self.last = None
            self.pos = None
            self.point = None
            self.dirs = {self['dirname']:100}
            self.note_ok = False
        elif num == 0 and num2 != 1:
            # No explicit request for direction, and this wasn't a
            # simple 'repeat-last', so move forward
            self.backwards = False

        self.do_parse()

        if self.backwards:
            if num2 == 1:
                n = self.find_prev()
            elif self.point:
                # First request to step backward, just repeat
                # current match.
                n = self.map[int(self.point['ref'])]
            else:
                n = None
            if not n:
                focus.call("Message", "No previous matches")
                self.backwards = False
                return 1
        else:
            n = self.find_next()
            while n and self.last and n == self.last:
                # Don't want to see the same line again
                n = self.find_next()
            if not n:
                focus.call("Message", "No further matches")
                # send viewers to keep following end of file.
                first_match = self.first_match
                self.first_match = True
                p = edlib.Mark(self)
                self.call("doc:set-ref", p)
                self.call("doc:notify:make-set-match", p, 1)
                if num2 or self['doc-status'] == 'Running' or not first_match:
                    # 'num2' means we are using a simple repeat-last-command
                    # 'doc-status' means this make/grep is still running.
                    # not first_match means we aren't at end-of-file, so stop there
                    # In either case stop here, don't try next make/grep doc
                    return 1
                return 0
        self.last = n
        if xy[0] > 0:
            # This isn't the first pane to be notified, but we found a new match
            # so move it to the top of the list.
            self.drop_notify("make:match-docs")
            self.call("editor:request:make:match-docs")
        if not str2:
            str2 = "AnyPane"
        try:
            self.goto_mark(focus, n, str2)
        except edlib.commandfailed:
            # error already reported
            pass
        self.call("doc:notify:make-set-match", self.point)
        return 1

    def goto_mark(self, focus, n, where):
        (fname, lineno) = n
        if fname in self.files:
            (d,v) = self.files[fname]
            mk, l = d.vmarks(v, self)
            while mk and int(mk['line']) < int(lineno):
                mk = mk.next()
            if mk and int(mk['line']) == int(lineno):
                self.visit(focus, d, mk, lineno, where)
                return 1
            self.visit(focus, d, None, lineno, where)
            return 1
        # try the old way
        dir = ""
        if fname[0] != '/':
            for d in self.dirs:
                if os.path.isfile(os.path.join(d, fname)):
                    dir = d
                    break
        try:
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
        # 'where' can be one of 'ThisPane' 'OtherPane' 'AnyPane' or 'PopupPane'
        # If 'AnyPane' or 'OtherPane' we ensure match view is visible
        # unless we were in a popup
        par = None; pane = None

        if focus.call("ThisPopup", ret='focus') == None:
            in_popup = False
        else:
            in_popup = True
        if where in ['OtherPane', 'AnyPane']:
            par = focus.call("DocLeaf", d, ret='focus')
            if par:
                pass
            elif where == 'OtherPane':
                pane = focus.call(where, ret='focus')
            else:
                pane = focus.call('ThisPane', ret='focus')
        elif where == 'ThisPane':
            pane = focus.call(where, ret='focus')
        elif where == 'PopupPane':
            pane = focus.call('ThisPopup', ret='focus')
            if not pane:
                pane = focus.call("PopupTile", "MD3ta", ret='focus')
        else:
            return edlib.Enoarg

        if not par and not pane:
            d.close()
            focus.call("Message", "Failed to open pane")
            return edlib.Efail
        if not par:
            par = d.call("doc:attach-view", pane, 1, ret='focus')
            if (in_popup and where == 'ThisPane') or where == 'PopupPane':
                p2 = par.call("attach-viewer", par, ret='focus')
                if p2:
                    par = p2;
        if not par:
            d.close()
            focus.call("Message", "Failed to open pane")
            return edlib.Efail
        par.take_focus()
        if mk and (int(lineno) == 1 or d.prior(mk) != None):
            par.call("Move-to", mk, 0, 1)
        else:
            # either no mark, or the mark has moved to start of doc, probably
            # due to a reload
            par.call("doc:file", -1)
            lineno = int(lineno)
            if lineno > 1:
                par.call("doc:EOL", lineno-1, 1)

        if not in_popup and where in ['AnyPane', 'OtherPane']:
            docpane = par.call("DocPane", self, ret='focus')
            if not docpane:
                docpane = par.call("OtherPane", ret='focus')
                if docpane:
                    self.call("doc:attach-view", docpane)
        return 1

    def handle_revisit(self, key, mark, **a):
        "handle:doc:make-revisit"
        self.do_parse()

        # clear out highlight
        self.clear_render()

        self.point = None
        self.note_ok = "First"
        p = self.vmark_at_or_before(self.viewnum, mark)
        if p:
            self.point = p.prev()

        self.last = None
        # move to front of match-docs list
        self.drop_notify("make:match-docs")
        self.call("editor:request:make:match-docs")
        return 1

    def handle_make_close(self, key, **a):
        "handle:make-close"
        # make output doc is being reused, so we'd better get out of the way,
        # unless still running
        if self['doc-status'] == 'Running':
            return 2
        # close the shellcmd handler, and that will close us
        self.parent.close()
        return 1

def make_attach(key, focus, comm2, str, str2, **a):
    p = focus.call("attach-shellcmd", 1, str, str2, ret='focus')
    if not p:
        return edlib.Efail
    focus['view-default'] = 'make-viewer'
    p = MakePane(p)
    if not p:
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
        self.may_follow = True

    def handle_set_match(self, key, mark, num, **a):
        "handle:make-set-match"
        self.call("Move-to", mark, 0, 1)
        # suppress following the next update
        self.may_follow = (num == 1)
        return 1

    def handle_enter(self, key, focus, mark, **a):
        "handle-list/K:Enter/K-o"
        dname = focus["doc-name"]
        doc = focus.call("docs:byname", dname, ret='focus')
        root = focus.call("RootPane", ret='focus')

        focus.call("doc:notify:doc:make-revisit", mark)
        next_match("interactive-cmd-next-match", focus,
                   edlib.NO_NUMERIC, "OtherPane", 0)
        # If this doc no longer visible but some other match doc
        # is, replace that doc with this one, to avoid confusion
        if doc and root:
            other = root.call("DocPane", doc, ret='focus')
            if not other:
                other = root.call("editor:notify:make:match-docs",
                                  "find-visible", ret='focus')
                if other:
                    doc.call("doc:attach-view", other)
        return 1

    def handle_find(self, key, focus, mark, **a):
        "handle:K-f"
        focus.call("doc:notify:doc:make-revisit", mark)
        next_match("interactive-cmd-next-match", focus,
                   edlib.NO_NUMERIC, "ThisPane", 0)
        return 1

    def handle_popup(self, key, focus, mark, **a):
        "handle:K-p"
        focus.call("doc:notify:doc:make-revisit", mark)
        next_match("interactive-cmd-next-match", focus,
                   edlib.NO_NUMERIC, "PopupPane", 0)
        return 1

    def handle_kill(self, key, **a):
        "handle:K-q"
        self.call("doc:destroy")
        return 1

    def handle_replace(self, key, focus, mark, mark2, num2, **a):
        "handle:doc:replaced"
        if not mark or not mark2 or num2:
            return 1
        if not self.may_follow:
            # only suppress one update
            self.may_follow = True
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
        if str == "render:make-line" and str2 == "other":
            comm2("attr:callback", focus, mark, "bg:cyan+80", 10000, 2)
            return 1
        if str == "render:make-line" and str2 == "best":
            comm2("attr:callback", focus, mark, "bg:magenta+80", 10000, 2)
            return 1

def make_view_attach(key, focus, comm2, **a):
    p = focus.call("attach-viewer", ret='focus')
    p = MakeViewerPane(p)

    if not p:
        return edlib.Efail
    if comm2:
        comm2("callback", p)
    return 1

class makeprompt(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.call("attach-file-entry", "shellcmd")

    def enter(self, key, focus, **a):
        "handle:K:Enter"
        str = focus.call("doc:get-str", ret="str")
        return focus.call("popup:close", str)

    def up(self, key, focus, **a):
        "handle:K:Up"
        d = self['dirname']
        if d[-1] == '/':
            d = os.path.dirname(d)
        d = os.path.dirname(d)
        if d[-1] != '/':
            d = d + '/'
        self['dirname'] = d
        self['pane-title'] = "%s in %s" % (self['cmd'],d)
        self['done-key'] = "N:%s:%s:%s" % (self['autosave'], self['mode'], d)
        # FIXME this only damaged the board content, not doc content
        # Is there a way I can communicate that?
        focus.leaf.call("view:changed")
        return 1

    def down(self, key, focus, **a):
        "handle:K:Down"
        d = self['dirname']
        orig = self['orig-dirname']
        if not orig.startswith(d):
            return 1
        n = orig.find('/', len(d))
        if n > 0:
            d = orig[:n+1]
        else:
            d = orig
        self['dirname'] = d
        self['pane-title'] = "%s in %s" % (self['cmd'],d)
        self['done-key'] = "N:%s:%s:%s" % (self['autosave'], self['mode'], d)
        focus.leaf.call("view:changed")
        return 1

def isword(c):
    return c and c.isalnum() or c == '_'

def run_make(key, focus, str, **a):
    # key is X:a:mode:dir
    # Where 'X' is 'Y' if save-all has been attepted, else 'N'
    #  'a' is 1 if save-all should be unconditional (auto), else 0
    #  mode is git or grep or make
    #  dir is directory to run in.
    k = key[4:]
    c = k.index(':')
    dir = k[c+1:]
    mode = k[:c]
    # pop-up has completed, check if we need to save-all
    # If we already have, key will start 'Y:', else 'N:'
    if key.startswith('N:'):
        testonly = 1 if key[2] == '0' else 0
        if focus.call("docs:save-all", 0, testonly, dir) != 1:
            p = focus.call("PopupTile", "DM", ret='focus')
            p['done-key'] = 'Y:' + key[2:]
            p['default'] = str
            p['only-here'] = dir
            p.call("popup:set-callback", run_make)
            p.call("docs:show-modified")
            return 1

    if mode == "git":
        cmd = "git-grep"
        docname = "*Grep Output*"
    elif mode == "grep" or mode == "TAGS" or mode == "quilt":
        cmd = "grep"
        docname = "*Grep Output*"
    else:
        cmd = "make"
        docname = "*Compile Output*"
    doc = None
    still_running = False
    if cmd == "make":
        # try to reuse old document
        doc = focus.call("editor:notify:make:match-docs", "choose-make", dir,
                         ret='focus')
        if doc:
            if doc.notify("make-close") > 1:
                # make is still running
                still_running = True
            else:
                doc.call("doc:clear")
    else:
        # Tell any extant grep documents that
        # a/ are no longer running, b/ have point at the end, c/ are not
        # visible; to close
        focus.call("editor:notify:make:match-docs", "close-idle")
    if not doc:
        # If doing 'grep', or if there is no make output doc, create new one.
        doc = focus.call("doc:from-text", docname, "", ret='focus')
        if not doc:
            return edlib.Efail

    doc['dirname'] = dir
    doc['view-default'] = 'make-viewer'
    doc['make-command'] = str
    pd = None; p = None
    if cmd == "make":
        pd = focus.call("DocPane", doc, ret='focus')
        if not pd:
            p = focus.call("OtherPane", doc, ret='focus')
    else:
        p = focus.call("PopupTile", "MD3ta", ret='focus')
    if not p and not pd:
        return edlib.Efail
    if not pd:
        doc.call("doc:attach-view", p, 1, ret='focus')

    if not still_running:
        p = doc.call("attach-makecmd", str, dir, ret='focus')
        if p:
            p['cmd'] = cmd
    return 1

def make_request(key, focus, num, num2, str, mark, **a):
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

        if num >= 0 and mode != "grep":
            # if we found a project-root, run command from there.
            if d and d[-1] != '/':
                d = d + '/'
            dir = d

    if cmd != "make" and focus['doc-type'] == "text":
        if not mark:
            mark = focus.call("doc:point", ret='mark')
    if cmd != "make" and focus['doc-type'] == "text" and mark:
        # choose the word under the cursor
        if not str:
            m1 = mark.dup()
            c = focus.prior(m1)
            while isword(c):
                focus.prev(m1)
                c = focus.prior(m1)
            m2 = mark.dup()
            c = focus.following(m2)
            while isword(c):
                focus.next(m2)
                c = focus.following(m2)
            str = focus.call("doc:get-str", m1, m2, ret='str')
        if str and not ('\n' in str):
            if not "'" in str:
                dflt_arg = "'" + str + "'"
            elif not '"' in str:
                dflt_arg = '"' + str + '"'
            else:
                dflt_arg = str

    autosave = 1 if num > 0 and num != edlib.NO_NUMERIC else 0
    if cmd == "make" and num2:
        # re-use previous run if directory is compatible
        make_cmd = None
        doc = focus.call("editor:notify:make:match-docs", "choose-make", dir,
                         ret='focus')
        if not doc:
            rdir = os.path.realpath(dir)
            if len(rdir) > 1:
                rdir += "/"
            if rdir != dir:
                doc = focus.call("editor:notify:make:match-docs",
                                 "choose-make", rdir, ret='focus')
        if doc:
            dir = doc['dirname']
            make_cmd = doc['make-command']
            run_make("N:%d:%s:%s"%(autosave,mode,dir), focus, make_cmd)
            return 1

    # Create a popup to ask for make command
    p = focus.call("PopupTile", "D2", dflt, ret="focus")
    if not p:
        return edlib.Efail
    if dflt_arg:
        # The '1' says to make this mark selection replaceable
        p.call("mode-set-mark", 1)
        p.call("Replace", dflt_arg)
    p.call("popup:set-callback", run_make)
    p["prompt"] = "%s Command" % cmd
    p["done-key"] = "N:%d:%s:%s" % (autosave, mode, dir)
    p["autosave"] = "%d" % autosave
    p.call("doc:set-name", "%s Command" % cmd)
    p['pane-title'] = "%s in %s" %(cmd, dir)
    p['cmd'] = cmd
    p['mode'] = mode
    if history:
        p = p.call("attach-history", history, "popup:close", ret='focus')
    if dir:
        p["dirname"] = dir
    p['orig-dirname'] = focus['dirname']
    makeprompt(p)
    return 1

def next_match(key, focus, num, str, num2, **a):
    if num == edlib.NO_NUMERIC:
        restart = 0
    elif num < 0:
        restart = -1 # means step backwards
    else:
        restart = 1
    if not focus.call("editor:notify:make:match-docs", "next-match",
                      str, restart, num2):
        focus.call("Message", "No next-match found")

    return 1

editor.call("global-set-command", "attach-makecmd", make_attach)
editor.call("global-set-command", "attach-make-viewer", make_view_attach)
editor.call("global-set-command", "interactive-cmd-make", make_request)
editor.call("global-set-command", "interactive-cmd-grep", make_request)
editor.call("global-set-command", "interactive-cmd-git-grep", make_request)
editor.call("global-set-command", "interactive-cmd-next-match", next_match)
