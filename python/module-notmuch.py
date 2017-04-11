# -*- coding: utf-8 -*-
# edlib module for working with "notmuch" email.
#
# Two document types:
# - search list: list of saved searches with count of 'current', 'unread',
#   and 'new' messages
# - message list: provided by notmuch-search, though probably with enhanced threads
#
# Messages and composition is handled by a separte 'email' module.  This module
# will send messages to that module and provide services for composing and delivery.
#
# These can be all in with one pane, with sub-panes, or can sometimes have a pane to
# themselves.
#
# saved search are stored in config file as "saved.foo". Some are special.
# "saved.current" selects messages that have not been archived, and are not spam
# "saved.unread" selects messages that should be highlighted. It is normally "tag:unread"
# "saved.new" selects new messages. Normally "tag:new not tag:unread"
# "saved.current-list" should be a conjunction of "saved:" searches.  They are listed
#  in the "search list" together with a count of 'current' and 'current/new' messages.
# "saved.misc-list" is a subsect of current-list for which saved:current should not
# be assumed.

from subprocess import Popen, PIPE
import re
import os
import notmuch
import json
import time
import fcntl

class notmuch_db():
    def __init__(self):
        self.lock_path = os.environ["HOME"]+"/.notmuch-config"
        self.want_write = False
        self.fd = None
        self.nest = 0

    def get_write(self):
        if self.want_write:
            return self
        if self.nest:
            return None
        self.want_write = True
        return self
    def __enter__(self):
        if self.nest:
            self.nest += 1
            return self.db
        self.fd = open(self.lock_path)
        self.nest = 1
        if self.want_write:
            fcntl.flock(self.fd.fileno(), fcntl.LOCK_EX);
            self.db = notmuch.Database(mode = notmuch.Database.MODE.READ_WRITE)
        else:
            fcntl.flock(self.fd.fileno(), fcntl.LOCK_SH);
            self.db = notmuch.Database()
        return self.db

    def __exit__(self, a,b,c):
        self.nest -= 1
        if self.nest:
            return
        self.db.close()
        self.fd.close()
        self.db = None
        self.fd = None

def take(name, place, args, default=None):
    if args[name] is not None:
        place.append(args[name])
    else:
        place.append(default)
    return 1

class searches:
    # Manage the saved searches
    # We read all searches from the config file and periodically
    # update some stored counts.
    #
    # This is used to present the search-list document.
    def __init__(self):
        self.slist = {}
        self.current = []
        self.misc = []
        self.count = {}
        self.unread = {}
        self.new = {}

        if 'NOTMUCH_CONFIG' in os.environ:
            self.path = os.environ['NOTMUCH_CONFIG']
        elif 'HOME' in os.environ:
            self.path = os.environ['HOME'] + "/.notmuch-config"
        else:
            self.path = ".notmuch-config"
        self.mtime = 0
        self.maxlen = 0

    def load(self, reload = False):
        try:
            stat = os.stat(self.path)
            mtime = stat.st_mtime
        except OSError:
            mtime = 0
        if not reload and mtime <= self.mtime:
            return False

        p = Popen("notmuch config list", shell=True, stdout=PIPE)
        if not p:
            return False
        self.slist = {}
        for line in p.stdout:
            if line[:6] != "saved.":
                continue
            w = line[6:].strip().split("=", 1)
            self.slist[w[0]] = w[1]
            if len(w[0]) > self.maxlen:
                self.maxlen = len(w[0])
        try:
            p.communicate()
        except IOError:
            pass
        if "current-list" not in self.slist:
            self.slist["current-list"] = "saved:inbox saved:unread"
            if "inbox" not in self.slist:
                self.slist["inbox"] = "tag:inbox"
            if "saved:unread" not in self.slist:
                self.slist["unread"] = "tag:unread"

        if "misc-list" not in self.slist:
            self.slist["misc-list"] = ""
        if "unread" not in self.slist:
            self.slist["unread"] = "tag:unread"
        if "new" not in self.slist:
            self.slist["new"] = "(tag:new AND tag:unread)"

        self.current = self.searches_from("current-list")
        self.misc = self.searches_from("misc-list")
        for i in self.current:
            if i not in self.count:
                self.count[i] = None
                self.unread[i] = None
                self.new[i] = None
        self.mtime = mtime
        return True

    def old_update(self):
        for i in self.current:
            q = notmuch.Query(self.db, self.make_search(i, False))
            self.count[i] = q.count_messages()
            q = notmuch.Query(self.db, self.make_search(i, 'unread'))
            self.unread[i] = q.count_messages()
            q = notmuch.Query(self.db, self.make_search(i, 'new'))
            self.new[i] = q.count_messages()

    def update(self, pane, cb):
        self.todo = [] + self.current
        self.pane = pane
        self.cb = cb
        return self.update_one()

    def update_one(self):
        if not self.todo:
            self.pane = None
            self.cb = None
            return False
        n = self.todo[0]
        self.p = Popen("/usr/bin/notmuch count --batch", shell=True, stdin=PIPE,
                       stdout = PIPE)
        self.p.stdin.write(self.make_search(n, False) + "\n")
        self.p.stdin.write(self.make_search(n, 'unread') + "\n")
        self.p.stdin.write(self.make_search(n, 'new') + "\n")
        self.p.stdin.close()
        self.pane.call("event:read", self.p.stdout.fileno(), self.cb)
        return True

    def updated(self, *a):
        if not self.todo:
            return False
        n = self.todo.pop(0)
        try:
            c = self.p.stdout.readline()
            self.count[n] = int(c)
            u = self.p.stdout.readline()
            self.unread[n] = int(u)
            nw = self.p.stdout.readline()
            self.new[n] = int(nw)
        except:
            pass
        p = self.p
        self.p = None
        more = self.update_one()
        p.wait()
        return more

    patn = "\\bsaved:([-_A-Za-z0-9]*)\\b"
    def map_search(self, query):
        m = re.search(self.patn, query)
        while m:
            s = m.group(1)
            if s in self.slist:
                q = self.slist[s]
                query = re.sub('saved:' + s,
                               '(' + q + ')', query)
            else:
                query = re.sub('saved:' + s,
                               'saved-'+s, query)
            m = re.search(self.patn, query)
        return query

    def make_search(self, name, extra):
        s = "saved:" + name
        if name not in self.misc:
            s = s + " saved:current"
        if extra:
            s = s + " saved:" + extra
        return self.map_search(s)

    def searches_from(self, n):
        ret = []
        if n in self.slist:
            for s in self.slist[n].split(" "):
                if s[:6] == "saved:":
                    ret.append(s[6:])
        return ret

class notmuch_main(edlib.Doc):
    # This is the document interface for the saved-search list.
    # It contain the searches as item which have attributes
    # providing name, count, unread-count
    # Once activated it auto-updates every 5 minutes
    def __init__(self, focus):
        edlib.Doc.__init__(self, focus, self.handle)
        self.searches = searches()
        self.timer_set = False
        self.updating = None
        self.seen_threads = {}
        self.seen_msgs = {}
        self.db = notmuch_db()

    def handle(self, key, focus, mark, mark2, numeric, extra, str, str2, comm2, **a):

        if key == "doc:revisit":
            return 1

        if key == "doc:set-ref":
            if numeric == 1:
                mark.offset = 0
            else:
                mark.offset = len(self.searches.current)
            mark.rpos = 0
            self.to_end(mark, numeric == 0);

            return 1

        if key == "doc:mark-same":
            return 1 if mark.offset == mark2.offset else 2

        if key == "doc:step":
            forward = numeric
            move = extra
            ret = edlib.WEOF
            target = mark
            if forward and mark.offset < len(self.searches.current):
                ret = ' '
                if move:
                    m2 = mark.next_any()
                    while m2 and m2.offset <= mark.offset + 1:
                        target = m2
                        m2 = m2.next_any()
                    o = mark.offset
                    mark.to_mark(target)
                    mark.offset = o+1
            if not forward and mark.offset > 0:
                ret = ' '
                if move:
                    m2 = mark.prev_any()
                    while m2 and m2.offset >= mark.offset - 1:
                        target = m2
                        m2 = m2.prev_any()
                    o = mark.offset
                    mark.to_mark(target)
                    mark.offset = o - 1
            return ret

        if key == "doc:get-attr":
            attr = str
            forward = numeric
            o = mark.offset
            if not forward:
                o -= 1
            val = None
            if o >= 0 and o < len(self.searches.current):
                s = self.searches.current[o]
                if attr == 'query':
                    val = s
                elif attr == 'fmt':
                    if self.searches.new[s]:
                        val = "bold,fg:red"
                    elif self.searches.unread[s]:
                        val = "bold,fg:blue"
                    elif self.searches.count[s]:
                        val = "fg:black"
                    else:
                        val = "fg:grey"
                elif attr == 'name':
                        val = "%-12s" % s
                elif attr == 'count':
                    c = self.searches.new[s]
                    if not c:
                        c = self.searches.unread[s]
                    if not c:
                        c = self.searches.count[s]
                    if c is None:
                        val = "%5s" % "?"
                    else:
                        val = "%5d" % c
            comm2("callback", focus, val)
            return 1

        if key == "doc:notmuch:update":
            if not self.timer_set:
                self.timer_set = True
                self.call("event:timer", 5*60, self.tick)
            self.searches.load(False)
            self.notify("Notify:Replace")
            self.updating = "counts"
            if not self.searches.update(self, self.updated):
                self.update_next()
            return 1

        if key == "doc:notmuch:query":
            # note: this is a private document that doesn't
            # get registered in the global list
            q = self.searches.make_search(str, None)
            nm = None
            it = self.children()
            if it:
                for child in it:
                    if child("doc:notmuch:same-search", str, q) > 0:
                        nm = child
                        break
            if not nm:
                nm = notmuch_list(self, q)
                nm.call("doc:set-name", str)
            if comm2:
                comm2("callback", nm)
            return 1

        if key == "doc:notmuch:byid":
            # return a document for the email message,
            # this is a global document
            with self.db as db:
                m = db.find_message(str)
                fn = m.get_filename() + ""
            pl = []
            focus.call("doc:open", "email:"+fn, -2, lambda key,**a:take('focus',pl,a))
            if pl:
                pl[0].call("doc:set-parent", self)
                comm2("callback", pl[0])
            return 1

        if key == "doc:notmuch:search-maxlen":
            return self.searches.maxlen + 1

        if key == "doc:notmuch:query-updated":
            self.update_next()
            return 1

        if key == "doc:notmuch:mark-read":
            with self.db.get_write() as db:
                m = db.find_message(str2)
                if m:
                    t = list(m.get_tags())
                    if "unread" in t:
                        m.remove_tag("unread")
                    if "new" in t:
                        m.remove_tag("new")
            return 1

        if key[:23] == "doc:notmuch:remove-tag-":
            tag = key[23:]
            with self.db.get_write() as db:
                if str2:
                    m = db.find_message(str2)
                    if m:
                        t = list(m.get_tags())
                        if tag in t:
                            m.remove_tag(tag)
                else:
                    q = db.create_query("thread:%s" % str)
                    for t in q.search_threads():
                        ml = t.get_messages()
                        for m in ml:
                            if tag in m.get_tags():
                                m.remove_tag(tag)
            return 1

        if key == "doc:notmuch:remember-seen-thread" and str:
            self.seen_threads[str] = focus
            return 1
        if key == "doc:notmuch:remember-seen-msg" and str:
            self.seen_msgs[str] = focus
            return 1
        if key == "doc:notmuch:mark-seen":
            with self.db.get_write() as db:
                todel = []
                for id in self.seen_msgs:
                    if self.seen_msgs[id] == focus:
                        m = db.find_message(id)
                        if m:
                            m.remove_tag("new")
                        todel.append(id)
                for id in todel:
                    del self.seen_msgs[id]
                todel = []
                for id in self.seen_threads:
                    if self.seen_threads[id] == focus:
                        q = db.create_query("thread:%s" % id)
                        for t in q.search_threads():
                            ml = t.get_messages()
                            for m in ml:
                                if "new" in m.get_tags():
                                    m.remove_tag("new")
                            break
                        todel.append(id)
                for id in todel:
                    del self.seen_threads[id]
            return 1

    def tick(self, key, **a):
        if not self.updating:
            self.updating = "counts"
            self.searches.load(False)
            if not self.searches.update(self, self.updated):
                self.update_next()
        return 1

    def updated(self, key, **a):
        if not self.searches.updated():
            self.update_next()
        self.notify("Notify:Replace")
        return -1

    def update_next(self):
        if self.updating == "counts":
            self.updating = "queries"
            qlist = []
            for c in self.children():
                qlist.append(c['query'])
            self.qlist = qlist
        while self.updating == "queries":
            if not self.qlist:
                self.updating = None
            else:
                q = self.qlist.pop(0)
                for c in self.children():
                    if c['query'] == q:
                        c("doc:notmuch:query-refresh")
                        return


def notmuch_doc(key, home, focus, comm2, **a):
    # Create the root notmuch document
    nm = notmuch_main(home)
    nm['render-default'] = "notmuch:master-view"
    nm.call("doc:set-name", "*Notmuch*")
    nm.call("global-multicall-doc:appeared-")
    nm.call("doc:notmuch:update")
    if comm2 is not None:
        comm2("callback", focus, nm)
    return 1

class notmuch_master_view(edlib.Pane):
    # This pane controls one visible instance of the notmuch application.
    # It will eventually manage the size and position of the 3 panes
    # and will provide common handling for keystrokes
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)
        self.maxlen = 0 # length of longest query name
        self.list_pane = None
        self.query_pane = None
        self.query = None
        self.message_pane = None
        pl = []
        self.call("attach-tile", "notmuch", "main", lambda key,**a:take('focus',pl,a))
        p = pl[-1]
        p.call("attach-view", lambda key,**a:take('focus',pl,a))
        p = pl[-1]
        p = p.render_attach("format")
        p = notmuch_main_view(p)
        self.list_pane = p

    def resize(self):
        if self.list_pane and (self.query_pane or self.message_pane):
            # list_pane must be no more than 25% total width, and no more than
            # 5+1+maxlen+1
            if self.maxlen <= 0:
                self.maxlen = self.call("doc:notmuch:search-maxlen")
                if self.maxlen > 1:
                    self.maxlen -= 1
            pl = []
            self.list_pane.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            tile = pl[0]
            space = self.w
            ch,ln = tile.scale()
            max = 5 + 1 + self.maxlen + 1
            if space * 100 / ch < max * 4:
                w = space / 4
            else:
                w = ch * 10 * max / 1000
            if tile.w != w:
                tile.call("Window:x+", "notmuch", w - tile.w)
        if self.query_pane and self.message_pane:
            # query_pane much be at least 4 lines, else 1/4 height
            # but never more than half the height
            pl = []
            self.query_pane.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            tile = pl[0]
            ch,ln = tile.scale()
            space = self.h
            min = 4
            if space * 100 / ln > min * 4:
                h = space / 4
            else:
                h = ln * 10 * min / 1000
                if h > space / 2:
                    h = space / 2
            if tile.h != h:
                tile.call("Window:y+", "notmuch", h - tile.h)

    def handle(self, key, focus, mark, numeric, str, str2, **a):

        in_message = False
        in_query = False
        in_main = False
        if self.message_pane and self.mychild(focus) == self.mychild(self.message_pane):
            in_message = True
        elif self.query_pane and self.mychild(focus) == self.mychild(self.query_pane):
            in_query = True
        else:
            in_main = True

        if key == "docs:choose":
            # don't choose anything
            return 1

        if key == "Clone":
            p = notmuch_master_view(focus)
            # We don't clone children, we create our own
            return 1

        if key == "Refresh:size":
            # First, make sure the tiler has adjusted to the new size
            self.focus.w = self.w
            self.focus.h = self.h
            self.focus("Refresh:size")
            # then make sure children are OK
            self.resize()
            return 1

        if key == "Chr-.":
            # select thing under point, but don't move
            focus.call("notmuch:select", mark, 0)
            return 1
        if key == "Return":
            # select thing under point, and enter it
            focus.call("notmuch:select", mark, 1)
            return 1
        if key == "Chr- ":
            if self.message_pane:
                self.message_pane.call(key)
            elif self.query_pane:
                self.query_pane.call("Return")
            else:
                self.list_pane.call("Return")
            return 1

        if key in [ "M-Chr-n", "M-Chr-p", "Chr-n", "Chr-p"]:
            if key[0] == "M" or not self.query_pane:
                p = self.list_pane
                op = self.query_pane
            else:
                p = self.query_pane
                op = self.message_pane
            if not p:
                return 1
            direction = 1 if key[-1] in "na" else -1
            m = mark
            if op:
                # secondary window exists, so move
                pl=[]
                p.call("doc:dup-point", 0, -2, lambda key,**a:take("mark", pl, a))
                m = pl[0]
                if p.call("Move-Line", direction, m) == 1:
                    p.call("Move-to", m)
                    p.damaged(edlib.DAMAGED_CURSOR)
            p.call("notmuch:select", m, 1)
            return 1

        if key == "Chr-a":
            if in_message:
                mp = self.message_pane
                if mp.cmid and mp.ctid:
                    if self.query_pane:
                        self.query_pane.call("doc:notmuch:remove-tag-inbox",
                                             mp.ctid, mp.cmid)
                    else:
                        self.call("doc:notmuch:remove-tag-inbox", mp.ctid, mp.cmid)
                self.call("Chr-n")
                return 1
            if in_query:
                sl = []
                focus.call("doc:get-attr", "thread-id", 1, mark, lambda key,**a:take('str',sl,a))
                focus.call("doc:get-attr", "message-id", 1, mark, lambda key,**a:take('str',sl,a))
                self.query_pane.call("doc:notmuch:remove-tag-inbox", sl[0], sl[1])
                # Move to next message.
                pl=[]
                focus.call("doc:dup-point", 0, -2, lambda key,**a:take("mark", pl, a))
                m = pl[0]
                if focus.call("Move-Line", 1, m) == 1:
                    focus.call("Move-to", m)
                    focus.damaged(edlib.DAMAGED_CURSOR)
                if self.message_pane:
                    # Message was displayed, so display this one
                    focus.call("notmuch:select", m, 0)
                return 1
            return 1

        if key == "doc:notmuch-close-message":
            self.message_pane = None
            return 1

        if key in [ "Chr-x", "Chr-q" ]:
            if self.message_pane:
                if key != "Chr-x":
                    self.mark_read()
                p = self.message_pane
                self.message_pane = None
                p.call("Window:close", "notmuch")
            elif self.query_pane:
                if key != "Chr-x":
                    self.query_pane.call("doc:notmuch:mark-seen")
                p = self.query_pane
                self.query_pane = None
                p.call("Window:close", "notmuch")
            else:
                pl=[]
                self.call("ThisPane", lambda key,**a:take('focus',pl, a))
                if pl and pl[0].focus:
                    pl[0].focus.close()
            return 1

        if key in [ "Chr-V" ]:
            if not self.message_pane:
                return 1
            pl = []
            self.call("OtherPane", lambda key,**a:take('focus', pl, a))
            if not pl:
                return 1
            pl[0].call("doc:attach", lambda key,**a:take('focus', pl, a))
            self.call("doc:open", self.message_pane["filename"], -1,
                       lambda key,**a:take('focus', pl, a))
            pl[2].call("doc:autoclose", 0)
            pl[1].call("doc:assign",pl[2], "default:viewer")
            return 1

        if key == "Chr-o":
            # focus to next window
            focus.call("Window:next", "notmuch", numeric)
            return 1

        if key == "Chr-g":
            focus.call("doc:notmuch:update")
            self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
            return 1

        if key == "notmuch:select-query":
            # A query was selected, identifed by 'str'.  Close the
            # message window and open a threads window.
            if self.message_pane:
                p = self.message_pane
                self.message_pane = None
                p.call("Window:close", "notmuch")
            if self.query_pane:
                self.query_pane.call("doc:notmuch:mark-seen")
            pl = []
            self.call("doc:notmuch:query", str,lambda key,**a:take('focus',pl,a))
            self.list_pane.call("OtherPane", "notmuch", "threads", 3,
                                    lambda key,**a:take('focus', pl, a))
            self.query_pane = pl[-1]
            pl[-1].call("doc:attach",
                        lambda key,**a:take('focus',pl,a))
            pl[-1].call("doc:assign", pl[0], "notmuch:threads",
                                    lambda key,**a:take('focus', pl, a))
            self.query_pane = pl[-1]
            if numeric:
                self.query_pane.take_focus()
            self.resize()
            return 1

        if key == "notmuch:select-message":
            # a thread or message was selected. id in 'str'. threadid in str2
            # Find the file and display it in a 'message' pane
            self.mark_read()
            pl=[]
            self.call("doc:notmuch:byid", str, lambda key,**a:take('focus',pl,a))
            self.query_pane.call("OtherPane", "notmuch", "message", 2,
                                 lambda key,**a:take('focus',pl,a))
            pl[-1].call("doc:attach",
                        lambda key,**a:take('focus',pl,a))
            pl[-1].call("doc:assign", pl[0], "notmuch:message",
                        lambda key,**a:take('focus', pl, a))

            # This still doesn't work: there are races: attaching a doc to
            # the pane causes the current doc to be closed.  But the new doc
            # hasn't been anchored yet so if they are the same, we lose.
            # Need a better way to anchor a document.
            #pl[0].call("doc:autoclose", 1);
            p = self.message_pane = pl[-1]
            p.ctid = str2
            p.cmid = str
            if numeric:
                self.message_pane.take_focus()
            self.resize()
            return 1

    def mark_read(self):
        p = self.message_pane
        if not p:
            return
        if self.query_pane:
            self.query_pane.call("doc:notmuch:mark-read", p.ctid, p.cmid)

class notmuch_main_view(edlib.Pane):
    # This pane provides view on the search-list document.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)
        self['render-wrap'] = 'no'
        self['background'] = 'color:#A0FFFF'
        self['line-format'] = '<%fmt>%count %+name</>'
        self.call("Request:Notify:Replace")
        self.maxlen = 0
        self.selected = None

    def handle(self, key, focus, mark, numeric, **a):
        if key == "Clone":
            p = notmuch_main_view(focus)
            self.clone_children(focus.focus)
            return 1
        if key == "Notify:Replace":
            self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
            return 0

        if key == "notmuch:select":
            sl = []
            focus.call("doc:get-attr", "query", mark, 1, lambda key,**a:take('str',sl,a))
            if sl and sl[0]:
                focus.call("notmuch:select-query", sl[0], numeric)
            return 1

def render_master_view_attach(key, focus, comm2, **a):
    # The master view for the '*Notmuch*' document uses multiple tiles
    # to display the available searches, the current search results, and the
    # current message, though each of these is optional.
    # The tile which displays the search list does not have a document, as it
    # refers down the main document.  So it doesn't automatically get borders
    # from a 'view', so we must add one explicitly.
    p = focus
    p = notmuch_master_view(focus)
    while p.focus:
        p = p.focus
    p.take_focus()
    comm2("callback", p)
    return 1

def notmuch_mode(key, home, focus, **a):
    pl=[]
    focus.call("ThisPane", lambda key, **a:take('focus', pl, a))
    try:
        home.call("docs:byname", "*Notmuch*", lambda key, **a:take('focus', pl, a))
    except:
        home.call("attach-doc-notmuch", lambda key, **a:take('focus', pl, a))
    if len(pl) != 2:
        return -1
    pl[0].call("doc:attach",
               lambda key,**a:take('focus', pl, a))
    pl[-1].call("doc:assign", pl[1], 1)
    return 1

##################
# list-view shows a list of threads/messages that match a given
# search query.
# We generate the thread-ids using "notmuch search --output=threads"
# For a full-scan we collect at most 100 and at most 1 month at a time, until
# we reach and empty month, then get all the rest together
# For an update, we just check the last day and add anything missing.
# We keep an array of thread-ids
#
# Three different views are presented of this document depending
# on extra arguments in the
#    doc:set-ref doc:mark-same doc:step doc:get-attr
# messages.
# By default, when 'str' is None, only the separate threads a visible as objects.
# If 'str' is a thread id and 'xy.x' is 0, then in place of that thread a
#   list of 'matching' message is given
# If 'str' is a thread id and 'xy.x' is non-zero, then only the messages in
#   the thread are visible, but all are visible, including non-matching threads

class notmuch_list(edlib.Doc):
    def __init__(self, focus, query):
        edlib.Doc.__init__(self, focus, self.handle)
        self.db = notmuch_db()
        self.query = query
        self['query'] = query
        self.threadids = []
        self.threads = {}
        self.messageids = {}
        self.threadinfo = {}
        self["render-default"] = "notmuch:query"
        self["line-format"] = "<%hilite>%date_relative</><tab:130></> <fg:blue>%+authors</><tab:350>%threadinfo<tab:450><%hilite>%subject</>                      "
        self.load_full()

    def load_full(self):
        self.old = self.threadids[:]
        self.new = []
        self.offset = 0
        self.age = 1
        self.partial = False
        self.start_load()

    def load_update(self):
        self.partial = True
        self.age = None
        self.old = self.threadids[:]
        self.new = []
        self.offset = 0
        self.start_load()

    def start_load(self):
        cmd = ["/usr/bin/notmuch", "search", "--output=summary", "--format=json", "--limit=100", "--offset=%d" % self.offset ]
        if self.partial:
            cmd += [ "date:-1day.. AND " ]
        elif self.age:
            cmd += [ "date:-%dmonths.. AND " % self.age]
        cmd += [ "( %s )" % self.query ]
        self.p = Popen(cmd, shell=False, stdout=PIPE)
        self.call("event:read", self.p.stdout.fileno(), self.get_threads)

    def get_threads(self, key, **a):
        found = 0
        try:
            tl = json.load(self.p.stdout)
        except:
            tl = []
        for j in tl:
            tid = j['thread']
            found += 1
            try:
                i = self.old.index(tid)
                #if i > 0:
                #    self.move_marks_from(tid)
                del self.old[i]
            except ValueError:
                pass
            if tid not in self.new:
                self.new.append(tid)
            self.threads[tid] = j
        tl = None
        if self.p:
            self.p.wait()
        self.p = None
        if not self.threadids and len(self.new):
            # first insertion, all marks must be at start
            m = self.first_mark()
            while m:
                if m.pos is None:
                    m.pos = (self.new[0],)
                m = m.next_any()
        self.threadids = self.new + self.old
        self.notify("Notify:Replace")
        if found < 100 and self.age == None:
            # must have found them all
            self.call("doc:notmuch:query-updated")
            return -1
        # request some more
        self.offset += found - 3
        if found < 5:
            # stop worrying about age
            self.age = None
            self.offset = 0
        if found < 100 and self.age:
            self.age += 1
        # allow for a little over-lap across successive calls
        self.start_load()
        return -1

    def add_message(self, m, lst, info, depth):
        mid = m.get_message_id()
        lst.append(mid)
        l = list(m.get_replies())
        depth += [ 1 if l else 0 ]
        info[mid] = (m.get_filename(), m.get_date(),
                     m.get_flag(notmuch.Message.FLAG.MATCH),
                     depth, m.get_header("From"), m.get_header("Subject"), list(m.get_tags()))
        depth = depth[:-1]
        if l:
            l.sort(key=lambda m:(m.get_date(), m.get_header("subject")))
            for m in l[:-1]:
                self.add_message(m, lst, info, depth + [1])
            self.add_message(l[-1], lst, info, depth + [0])

    def load_thread(self, tid):
        with self.db as db:
            q = notmuch.Query(db, "thread:%s and (%s)" % (tid, self.query))
            tl = list(q.search_threads())
            if not tl:
                return
            thread = tl[0]
            midlist = []
            minfo = {}
            ml = list(thread.get_toplevel_messages())
            ml.sort(key=lambda m:(m.get_date(), m.get_header("subject")))
            for m in list(ml):
                self.add_message(m, midlist, minfo, [2])
            self.messageids[tid] = midlist
            self.threadinfo[tid] = minfo

    def rel_date(self, sec):
        then = time.localtime(sec)
        now = time.localtime()
        nows = time.time()
        if sec < nows and sec > nows - 60:
            val = "%d secs. ago" % (nows - sec)
        elif sec < nows and sec > nows - 60*60:
            val = "%d mins. ago" % ((nows - sec)/60)
        elif sec < nows and sec > nows - 60*60*6:
            mn = (nows - sec) / 60
            hr = int(mn/60)
            mn -= 60*hr
            val = "%dh%dm ago" % (hr,mn)
        elif then[:3] == now[:3]:
            val = time.strftime("Today %H:%M", then)
        elif sec > nows:
            val = time.strftime("%D %T!", then)
        elif sec > nows - 7 * 24 * 3600:
            val = time.strftime("%a %H:%M", then)
        elif then[0] == now[0]:
            val = time.strftime("%d/%b %H:%M", then)
        else:
            val = time.strftime("%Y-%b-%d", then)
        val = "              " + val
        val = val[-13:]
        return val

    def pos_index(self, pos, visible, whole_thread):
        # return (threadnum, messagenum, moved, pos)
        # by finding the first position at or after pos
        # and 'threadnum' being index into self.threadids
        # 'messagenum' being -1 if thread not in visible, else
        # index into self.messageids[threadid',
        # 'moved' being true of 'pos' wasn't visible, and
        # 'pos' being the new position.
        # End-of-file is -1 -1 None
        if pos is None:
            return (-1, -1, False, None)
        th = pos[0]
        moved = False
        i = self.threadids.index(th)
        j = -1
        while whole_thread and th not in visible:
            # invisible thread, move to next
            moved = True
            i += 1
            if i >= len(self.threadids):
                return (-1, -1, moved, None)
            th = self.threadids[i]
            pos = (th,)
        # This thread is a valid location
        if th not in visible:
            if len(pos) == 2:
                # move to whole of thread
                pos = (th,)
                moved = True
            return (i, -1, moved, pos)
        # Thread is open, need to find a valid message, either after or before
        # FIXME I get a key-error below sometimes.  Maybe out-of-sync with DB
        mi = self.messageids[th]
        ti = self.threadinfo[th]
        if len(pos) == 1:
            j = 0
            moved = True
        else:
            j = mi.index(pos[1])
        while j < len(mi):
            if whole_thread or ti[mi[j]][2]:
                return (i, j, moved, (th, mi[j]))
            j += 1
            moved = True
        # search backwards
        while j > 0:
            j -= 1
            if whole_thread or ti[mi[j]][2]:
                return (i, j, True, (th, mi[j]))
        # IMPOSSIBLE
        return (-1, -1, True, None)

    def next(self, i, j, visible, whole_thread):
        # 'visible' is a list of visible thread-ids.
        # We move pos forward either to the next message in a visible thread
        # or the first message of the next thread
        # Return index into thread list and index into message list of original pos,
        # and new pos.
        # Normally only select messages which 'match' (info[2])
        # If whole_thread, then only select messages in a visible thread, and
        #  select any of them, including non-matched.
        # return i,j,pos
        if j == -1 and i >= 0:
            i += 1
        else:
            j += 1
        while True:
            if i == -1 or i >= len(self.threadids):
                return (-1, -1, None)
            th = self.threadids[i]
            if th in visible:
                # thread is visible, move to next message
                mi = self.messageids[th]
                ti = self.threadinfo[th]
                if j < 0:
                    j = 0
                while j < len(mi) and not whole_thread and not ti[mi[j]][2]:
                    j += 1
                if j < len(mi):
                    return (i, j, (th, mi[j]))
            elif not whole_thread:
                return (i, -1, (th,))
            # Move to next thread
            i += 1
            j = -1

    def prev(self, i, j, visible, whole_thread):
        # 'visible' is a list of visible thread-ids
        # We move pos backward either to previous message in a visible thread
        # or the last message of the previous thread.
        # Return index into thread list, index into message list, and new pos
        # (Indexs are new pos!)
        # Normally only select messages which 'match' (info[2])
        # If whole_thread, then only select messages in a visible thread,
        #   and select any of them, including non-matched.
        if i < 0:
            i = len(self.threadids) - 1
            j = -1;
        elif j <= 0:
            j = -1
            i -= 1
        else:
            j -= 1
        while True:
            if i < 0:
                return (-1, -1, None)
            th = self.threadids[i]
            if th in visible:
                mi = self.messageids[th]
                ti = self.threadinfo[th]
                if j < 0 or j >= len(mi):
                    j = len(mi) -1
                while j >= 0 and not whole_thread and not ti[mi[j]][2]:
                    j -= 1
                if j >= 0:
                    return (i, j, (th, mi[j]))
            elif not whole_thread:
                return (i, -1, (th,))
            # move to previous thread
            i -= 1
            j = -1


    def cvt_depth(self, depth):
        ret = ""

        for level in depth[:-2]:
            ret += u" │ "[level]
        ret += u"╰├─"[depth[-2]]
        ret += u"─┬"[depth[-1]]

        return ret + "> "

    def handle(self, key, mark, mark2, numeric, extra, focus, xy, str, str2, comm2, **a):
        if key == "doc:set-ref":
            mark.pos = None
            if numeric == 1 and len(self.threadids) > 0:
                i,j,moved,mark.pos = self.pos_index((self.threadids[0],),[str2], str2 and xy[0])
            mark.offset = 0
            mark.rpos = 0
            self.to_end(mark, numeric == 0)
            return 1

        if key == "doc:mark-same":
            if mark.pos == mark2.pos:
                return 1
            i1,j1,moved1,pos1 = self.pos_index(mark.pos, [str2], str2 and xy[0])
            i2,j2,moved2,pos2 = self.pos_index(mark2.pos, [str2], str2 and xy[0])
            return 1 if (i1,j1)==(i2,j2) else 2

        if key == "doc:step":
            forward = numeric
            move = extra
            ret = edlib.WEOF
            i,j,moved,pos = self.pos_index(mark.pos, [str2], str2 and xy[0])
            if moved:
                mark.pos = pos
            if forward:
                i2,j2,pos = self.next(i, j, [str2], str2 and xy[0])
                if mark.pos is not None:
                    ret = ' '
                if move:
                    m2 = mark.next_any()
                    target = None
                    while m2:
                        i3,j3,moved3,pos3 = self.pos_index(m2.pos, [str2], str2 and xy[0])
                        if (i3,j3) >= (i2,j2):
                            break
                        target = m2
                        m2 = m2.next_any()
                    if target:
                        mark.to_mark(target)
                    mark.pos = pos
            if not forward:
                i2,j2,pos = self.prev(i,j, [str2], str2 and xy[0])
                if i2 >= 0:
                    ret = ' '
                    if move:
                        m2 = mark.prev_any()
                        target = None
                        while m2:
                            i3,j3,moved3,pos3 = self.pos_index(m2.pos, [str2], str2 and xy[0])
                            if (i3,j3) <= (i2,j2):
                                break
                            target = m2
                            m2 = m2.prev_any()
                        if target:
                            mark.to_mark(target)
                        mark.pos = pos
            return ret

        if key == "doc:get-attr":
            attr = str
            forward = numeric
            i,j,moved,pos = self.pos_index(mark.pos, [str2], str2 and xy[0])
            if moved:
                mark.pos = pos
            if not forward:
                i,j,newpos = self.prev(i, j, [str2], str2 and xy[0])

            if attr in ["message-id","thread-id"]:
                val = None
            else:
                val = "["+attr+"]"
            if i >= 0 and j == -1 and self.threadids[i] != str2:
                # report on thread, not message
                tid = self.threadids[i]
                t = self.threads[tid]
                if attr == "message-id":
                    val = None
                elif attr == "thread-id":
                    val = tid
                elif attr == "hilite":
                    if "new" in t["tags"] and "unread" in t["tags"]:
                        val = "fg:red,bold"
                    elif "unread" in t["tags"]:
                        val = "fg:blue"
                    elif "inbox" not in t["tags"]:
                        # FIXME this test is wrong once we have generic searches
                        val = "fg:grey"
                    else:
                        val = "fg:black"
                elif attr == "date_relative":
                    val = self.rel_date(t['timestamp'])
                elif attr == "threadinfo":
                    val = "[%d/%d]" % (t['matched'],t['total'])
                elif attr in t:
                    val = t[attr]
                    if type(val) == int:
                        val = "%d" % val
                    else:
                        val = unicode(t[attr])
                    if attr == 'date_relative':
                        val = "           " + val
                        val = val[-13:]
                    if attr == "authors":
                        val = val[:20]

            if j >= 0 or (i >= 0 and self.threadids[i] == str2):
                # report on an individual message
                if j < 0:
                    j = 0
                tid = self.threadids[i]
                mid = self.messageids[tid][j]
                m = self.threadinfo[tid][mid]
                (fn, dt, matched, depth, author, subj, tags) = m
                if attr == "message-id":
                    val = mid
                elif attr == "thread-id":
                    val = tid
                elif attr == "hilite":
                    # FIXME this inbox test is wrong once we allow generic searches
                    if not matched or "inbox" not in tags:
                        val = "fg:grey"
                        if "new" in tags and "unread" in tags:
                            val = "fg:pink"
                    elif "new" in tags and "unread" in tags:
                        val = "fg:red,bold"
                    elif "unread" in tags:
                        val = "fg:blue"
                    else:
                        val = "fg:black"
                elif attr == "date_relative":
                    val = self.rel_date(dt)
                elif attr == "authors":
                    val = author[:20]
                elif attr == "subject":
                    val = subj
                elif attr == "threadinfo":
                    val = self.cvt_depth(depth)

            comm2("callback", focus, val)
            return 1

        if key == "doc:notmuch:load-thread":
            if str not in self.threadinfo:
                self.load_thread(str)
            return 1

        if key == "doc:notmuch:same-search":
            if self.query == str2:
                return 1
            return 0

        if key == "doc:notmuch:query-refresh":
            self.load_update()

        if key == "doc:notmuch:mark-read":
            ti = self.threadinfo[str]
            m = ti[str2]
            tags = m[6]
            if "unread" not in tags and "new" not in tags:
                return
            if "unread" in tags:
                tags.remove("unread")
            if "new" in tags:
                tags.remove("new")
            is_unread = False
            for mid in ti:
                if "unread" in ti[mid][6]:
                    # still has unread messages
                    is_unread = True
                    break
            if not is_unread:
                # thread is no longer 'unread'
                j = self.threads[str]
                t = j["tags"]
                if "unread" in t:
                    t.remove("unread")
            self.notify("Notify:Replace")
            # Let this fall though to database document.
            return 0

        if key[:23] == "doc:notmuch:remove-tag-":
            tag = key[23:]
            t = self.threads[str]
            if tag in t["tags"]:
                t["tags"].remove(tag)
            if not str2 or str not in self.threadinfo:
                return 0

            ti = self.threadinfo[str]
            m = ti[str2]
            tags = m[6]
            if tag not in tags:
                return 0
            tags.remove(tag)

            is_tagged = False
            for mid in ti:
                if tag in ti[mid][6]:
                    # still has tagged messages
                    is_tagged = True
                    break
            if not is_tagged:
                # thread is no longer tagged
                j = self.threads[str]
                t = j["tags"]
                if tag in t:
                    t.remove(tag)
            self.notify("Notify:Replace")
            # Let this fall though to database document.
            return 0


class notmuch_query_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)
        self.selected = None
        self.whole_thread = 0
        self.seen_threads = {}
        self.seen_msgs = {}

    def handle(self, key, focus, mark, mark2, numeric, **a):
        if key == "Clone":
            p = notmuch_query_view(focus)
            self.clone_children(focus.focus)
            return 1

        if key in ["doc:step", "doc:get-attr", "doc:mark-same"]:
            s = a['str']
            if s is None:
                s = ""
            del a['str']
            del a['str2']
            del a['home']
            del a['comm']
            del a['xy']
            return self.parent.call(key, focus, mark, mark2, numeric, s, self.selected,
                                    (self.whole_thread, 0), *(a.values()))

        if key == 'Chr-Z':
            self.whole_thread = 1 - self.whole_thread
            focus.damaged(edlib.DAMAGED_VIEW)
            focus.damaged(edlib.DAMAGED_CONTENT)
            return 1
        if key == "notmuch:select":
            sl = []
            focus.call("doc:get-attr", "thread-id", 1, mark, lambda key,**a:take('str',sl,a))
            if sl[0] != self.selected:
                self.call("doc:notmuch:load-thread", sl[0])
                self.selected = sl[0]
                focus.damaged(edlib.DAMAGED_VIEW)
                focus.damaged(edlib.DAMAGED_CONTENT)
            focus.call("doc:get-attr", "message-id", 1, mark, lambda key,**a:take('str',sl,a))
            if len(sl) == 2 and sl[-1] and numeric >= 0:
                focus.call("notmuch:select-message", sl[-1], sl[0], numeric)
            return 1

        if key == "render:reposition":
            # some messages have been displayed, from mark to mark2
            # collect threadids and message ids
            if not mark or not mark2:
                return 0
            m = mark.dup()

            while m < mark2:
                i = []
                focus.call("doc:get-attr", "thread-id", 1, m, lambda key,**a:take('str',i,a))
                focus.call("doc:get-attr", "message-id", 1, m, lambda key,**a:take('str',i,a))
                if i[0] and not i[1] and i[0] not in self.seen_threads:
                    self.seen_threads[i[0]] = True
                if i[0] and i[1]:
                    if i[0] in self.seen_threads:
                        del self.seen_threads[i[0]]
                    if  i[1] not in self.seen_msgs:
                        self.seen_msgs[i[1]] = True
                if edlib.WEOF == focus.call("doc:step", 1, 1, m):
                    break

        if key == "doc:notmuch:mark-seen":
            for i in self.seen_threads:
                self.parent.call("doc:notmuch:remember-seen-thread", i)
            for i in self.seen_msgs:
                self.parent.call("doc:notmuch:remember-seen-msg", i)
            self.parent.call("doc:notmuch:mark-seen")
            return 1

class notmuch_message_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)

    def handle(self, key, focus, mark, numeric, str, str2, comm2, **a):
        if key == "Close":
            self.call("doc:notmuch-close-message")
            return 1
        if key == "Clone":
            p = notmuch_message_view(focus)
            self.clone_children(focus.focus)
            return 1
        if key == "Replace":
            return 1
        if key == "Chr- ":
            focus.call("Next", 1, mark)
            # FIXME detect EOF and move to next message
            return 1
        if key == "map-attr" and str == "render:rfc822header":
            comm2("attr:callback", focus, int(str2), mark, "fg:#6495ed", 21)
            comm2("attr:callback", focus, 10000, mark, "wrap-tail: ,wrap-head:    ", 19)
            return 1
        if key == "map-attr" and str == "render:rfc822header-wrap":
            comm2("attr:callback", focus, int(str2), mark, "wrap", 20)
            return 1
        if key == "map-attr" and str == "render:rfc822header-subject":
            comm2("attr:callback", focus, int(str2), mark, "fg:blue,bold", 20)
            return 1
        if key == "map-attr" and str == "render:rfc822header-to":
            comm2("attr:callback", focus, int(str2), mark, "fg:blue,bold", 20)
            return 1

def render_query_attach(key, home, focus, comm2, **a):
    p = focus.render_attach("format")
    p = notmuch_query_view(p)
    if comm2:
        comm2("callback", p)
    return 1

def render_message_attach(key, home, focus, comm2, **a):
    p = focus.render_attach()
    p = notmuch_message_view(p)
    if comm2:
        comm2("callback", p)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "attach-doc-notmuch", notmuch_doc)
    editor.call("global-set-command", "attach-render-notmuch:master-view",
                render_master_view_attach)
    editor.call("global-set-command", "attach-render-notmuch:threads",
                render_query_attach)
    editor.call("global-set-command", "attach-render-notmuch:message",
                render_message_attach)
    editor.call("global-set-command", "interactive-cmd-nm", notmuch_mode)
