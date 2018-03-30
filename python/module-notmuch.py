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
    # This class is designed to be used with "with ... as" and provides
    # locking within the body o the clause.

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
        self.todo = []
        self.p = None

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

    def update(self, pane, cb):
        for i in self.current:
            if i not in self.todo:
                self.todo.append(i)
        self.pane = pane
        self.cb = cb
        if self.p is None:
            return self.update_next()
        return False

    def update_one(self, search, pane, cb):
        if search in self.todo:
            self.todo.remove(search)
        self.todo.insert(0, search)
        self.pane = pane
        self.cb = cb
        if self.p is None:
            self.update_next()

    def update_next(self):
        if not self.todo:
            self.pane = None
            self.cb = None
            return False
        n = self.todo.pop(0)
        self.todoing = n
        # HACK WARNING .bin and /dev/null
        self.p = Popen("/usr/bin/notmuch.bin count --batch", shell=True, stdin=PIPE,
                       stdout = PIPE, stderr = open("/dev/null", 'w'))
        self.p.stdin.write(self.make_search(n, False) + "\n")
        self.p.stdin.write(self.make_search(n, 'unread') + "\n")
        self.p.stdin.write(self.make_search(n, 'new') + "\n")
        self.p.stdin.close()
        self.pane.call("event:read", self.p.stdout.fileno(), self.cb)
        return True

    def updated(self, *a):
        if not self.todoing:
            return False
        n = self.todoing
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
        more = self.update_next()
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
    # It contains the searches as items which have attributes
    # providing name, count, unread-count
    # Once activated it auto-updates every 5 minutes
    def __init__(self, focus):
        edlib.Doc.__init__(self, focus)
        self.searches = searches()
        self.timer_set = False
        self.updating = None
        self.seen_threads = {}
        self.seen_msgs = {}
        self.db = notmuch_db()

    def handle_close(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:Close"
        return 1

    def handle_revisit(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:revisit"
        # Individual search-result documents are children of this
        # document, and we don't want doc:revisit from them to escape
        # to the global document list
        return 1

    def handle_set_ref(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:set-ref"
        if num == 1:
            mark.offset = 0
        else:
            mark.offset = len(self.searches.current)
        self.to_end(mark, num == 0);
        return 1

    def handle_step(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:step"
        forward = num
        move = num2
        ret = edlib.WEOF
        target = mark
        if forward and mark.offset < len(self.searches.current):
            ret = '\n'
            if move:
                o = mark.offset + 1
                m2 = mark.next_any()
                while m2 and m2.offset <= o:
                    target = m2
                    m2 = m2.next_any()
                mark.to_mark(target)
                mark.offset = o
        if not forward and mark.offset > 0:
            ret = '\n'
            if move:
                o = mark.offset - 1
                m2 = mark.prev_any()
                while m2 and m2.offset >= o:
                    target = m2
                    m2 = m2.prev_any()
                mark.to_mark(target)
                mark.offset = o
        return ret

    def handle_doc_get_attr(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:get-attr"
        attr = str
        o = mark.offset
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
                val = s
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
        if val:
            comm2("callback", focus, val)
            return 1
        return 0

    def handle_get_attr(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:get-attr"
        if comm2:
            if str == "doc-type":
                comm2("callback", focus, "notmuch")
                return 1
        return 0

    def handle_notmuch_update(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:update"
        if not self.timer_set:
            self.timer_set = True
            self.call("event:timer", 5*60, self.tick)
        self.searches.load(False)
        self.notify("Notify:doc:Replace")
        self.updating = "counts"
        if not self.searches.update(self, self.updated):
            self.update_next()
        return 1

    def handle_notmuch_update_one(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:update-one"
        self.searches.update_one(str, self, self.updated)
        return 1

    def handle_notmuch_query(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:query"
        # Find or create a search-result document as a
        # child of this document - it remains private
        # and doesn't get registered in the global list
        q = self.searches.make_search(str, None)
        nm = None
        it = self.children()
        for child in it:
            if child("doc:notmuch:same-search", str, q) == 1:
                nm = child
                break
        if not nm:
            nm = notmuch_list(self, str, q)
            nm.call("doc:set-name", str)
        if comm2:
            comm2("callback", nm)
        return 1

    def handle_notmuch_byid(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:byid"
        # Return a document for the email message.
        # This is a global document.
        with self.db as db:
            m = db.find_message(str)
            fn = m.get_filename() + ""
        doc = focus.call("doc:open", "email:"+fn, -2, ret='focus')
        if doc:
            doc.call("doc:set-parent", self)
            comm2("callback", doc)
        return 1

    def handle_notmuch_byid_tags(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:byid:tags"
        # return a string with tags of message
        with self.db as db:
            m = db.find_message(str)
            tags = ",".join(m.get_tags())
        comm2("callback", focus, tags)
        return 1

    def handle_notmuch_bythread_tags(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:bythread:tags"
        # return a string with tags of all messages in thread
        with self.db as db:
            q = db.create_query("thread:%s" % str)
            tg = []
            for t in q.search_threads():
                ml = t.get_messages()
                for m in ml:
                    tg.extend(m.get_tags())
        tags = ",".join(set(tg))
        comm2("callback", focus, tags)
        return 1

    def handle_notmuch_search_max(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:search-maxlen"
        return self.searches.maxlen + 1

    def handle_notmuch_query_updated(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:query-updated"
        # A child search document has finished updating.
        self.update_next()
        return 1

    def handle_notmuch_mark_read(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:mark-read"
        with self.db.get_write() as db:
            m = db.find_message(str2)
            if m:
                changed=False
                t = list(m.get_tags())
                if "unread" in t:
                    m.remove_tag("unread")
                    changed=True
                if "new" in t:
                    m.remove_tag("new")
                    changed=True
                if changed:
                    self.notify("Notify:Tag", str, str2)
        return 1

    def handle_notmuch_remove_tag(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle-range/doc:notmuch:remove-tag-/doc:notmuch:remove-tag./"
        tag = key[23:]
        with self.db.get_write() as db:
            if str2:
                # remove just from 1 message
                m = db.find_message(str2)
                if m:
                    t = list(m.get_tags())
                    if tag in t:
                        m.remove_tag(tag)
                        self.notify("Notify:Tag", str, str2)
            else:
                # remove from whole thread
                q = db.create_query("thread:%s" % str)
                changed = False
                for t in q.search_threads():
                    ml = t.get_messages()
                    for m in ml:
                        if tag in m.get_tags():
                            m.remove_tag(tag)
                            changed = True
                if changed:
                    self.notify("Notify:Tag", str)
        return 1

    def handle_notmuch_remember_seen_thread(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:remember-seen-thread"
        if str:
            self.seen_threads[str] = focus
            return 1

    def handle_notmuch_remember_seen_msg(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:remember-seen-msg"
        if str:
            self.seen_msgs[str] = focus
            return 1

    def handle_notmuch_mark_seen(self, key, focus, mark, mark2, num, num2, str, str2, comm2, **a):
        "handle:doc:notmuch:mark-seen"
        with self.db.get_write() as db:
            todel = []
            for id in self.seen_msgs:
                if self.seen_msgs[id] == focus:
                    m = db.find_message(id)
                    if m and "new" in m.get_tags():
                        m.remove_tag("new")
                        self.notify("Notify:Tag", m.get_thread_id(), id)
                    todel.append(id)
            for id in todel:
                del self.seen_msgs[id]
            todel = []
            for id in self.seen_threads:
                if self.seen_threads[id] == focus:
                    changed = False
                    q = db.create_query("thread:%s" % id)
                    for t in q.search_threads():
                        ml = t.get_messages()
                        for m in ml:
                            if "new" in m.get_tags():
                                m.remove_tag("new")
                                changed = True
                        break
                    todel.append(id)
                self.notify("Notify:Tag", id)
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
        self.notify("Notify:doc:Replace")
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

class notmuch_master_view(edlib.Pane):
    # This pane controls one visible instance of the notmuch application.
    # It manages the size and position of the 3 panes and provides common
    # handling for some keystrokes.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.maxlen = 0 # length of longest query name in list_pane
        self.list_pane = None
        self.query_pane = None
        self.message_pane = None

        p = self.call("attach-tile", "notmuch", "main", ret='focus')
        p = p.call("attach-view", ret='focus')
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
            tile = self.list_pane.call("ThisPane", "notmuch", ret='focus')
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
            # but never more than 1/2 the height
            tile = self.query_pane.call("ThisPane", "notmuch", ret='focus')
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


    def handle_choose(self, key, focus, mark, num, str, str2, **a):
        "handle:docs:choose"
        # don't choose anything
        return 1

    def handle_clone(self, key, focus, mark, num, str, str2, **a):
        "handle:Clone"
        p = notmuch_master_view(focus)
        # We don't clone children, we create our own
        return 1

    def handle_size(self, key, focus, mark, num, str, str2, **a):
        "handle:Refresh:size"
        # First, make sure the tiler has adjusted to the new size
        self.focus.w = self.w
        self.focus.h = self.h
        self.focus("Refresh:size")
        # then make sure children are OK
        self.resize()
        return 1

    def handle_dot(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr-."
        # select thing under point, but don't move
        focus.call("notmuch:select", mark, 0)
        return 1

    def handle_return(self, key, focus, mark, num, str, str2, **a):
        "handle:Return"
        # select thing under point, and enter it
        focus.call("notmuch:select", mark, 1)
        return 1

    def handle_space(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr- "
        if self.message_pane:
            self.message_pane.call(key)
        elif self.query_pane:
            self.query_pane.call("Return")
        else:
            self.list_pane.call("Return")
        return 1

    def handle_move(self, key, focus, mark, num, str, str2, **a):
        "handle-list/M-Chr-n/M-Chr-p/Chr-n/Chr-p"
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
            m = p.call("doc:dup-point", 0, -2, ret='mark')
            if p.call("Move-Line", direction, m) == 1:
                p.call("Move-to", m)
                p.damaged(edlib.DAMAGED_CURSOR)
        p.call("notmuch:select", m, 1)
        return 1

    def handle_A(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr-a"
        in_message = False
        in_query = False
        in_main = False
        if self.message_pane and self.mychild(focus) == self.mychild(self.message_pane):
            in_message = True
        elif self.query_pane and self.mychild(focus) == self.mychild(self.query_pane):
            in_query = True
        else:
            in_main = True

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
            thid = focus.call("doc:get-attr", "thread-id", mark, ret = 'str')
            msid = focus.call("doc:get-attr", "message-id", mark, ret = 'str')
            self.query_pane.call("doc:notmuch:remove-tag-inbox", thid, msid)
            # Move to next message.
            m = focus.call("doc:dup-point", 0, -2, ret='mark')
            if focus.call("Move-Line", 1, m) == 1:
                focus.call("Move-to", m)
                focus.damaged(edlib.DAMAGED_CURSOR)
            if self.message_pane:
                # Message was displayed, so display this one
                focus.call("notmuch:select", m, 0)
            return 1
        return 1

    def handle_close_message(self, key, focus, mark, num, str, str2, **a):
        "handle:notmuch-close-message"
        self.message_pane = None
        return 1

    def handle_xq(self, key, focus, mark, num, str, str2, **a):
        "handle-list/Chr-x/Chr-q"
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

            s = p.call("get-attr", "qname", 1, ret='str')
            if s:
                p.call("doc:notmuch:update-one", s)

            p.call("Window:close", "notmuch")
        else:
            p = self.call("ThisPane", ret='focus')
            if p and p.focus:
                p.focus.close()
        return 1

    def handle_v(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr-V"
        if not self.message_pane:
            return 1
        p2 = self.call("doc:open", self.message_pane["filename"], -1,
                       ret = 'focus')
        p2.call("doc:set:autoclose", 1)
        p0 = self.call("OtherPane", 4, p2, ret='focus')
        if not p0:
            return 1
        p1 = p0.call("doc:attach", ret='focus')
        p1.call("doc:assign",p2, "default:viewer")
        return 1

    def handle_o(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr-o"
        # focus to next window
        focus.call("Window:next", "notmuch")
        return 1

    def handle_O(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr-O"
        # focus to prev window
        focus.call("Window:prev", "notmuch")
        return 1

    def handle_g(self, key, focus, mark, num, str, str2, **a):
        "handle:Chr-g"
        focus.call("doc:notmuch:update")
        self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
        return 1

    def handle_select_query(self, key, focus, mark, num, str, str2, **a):
        "handle:notmuch:select-query"
        # A query was selected, identifed by 'str'.  Close the
        # message window and open a threads window.
        if self.message_pane:
            p = self.message_pane
            self.message_pane = None
            p.call("Window:close", "notmuch")
        if self.query_pane:
            self.query_pane.call("doc:notmuch:mark-seen")
        if self.query_pane:
            s = self.query_pane.call("get-attr", "qname", 1, ret='str')
            if s:
                self.list_pane.call("doc:notmuch:update-one", s)

        p0 = self.call("doc:notmuch:query", str, ret='focus')
        p1 = self.list_pane.call("OtherPane", "notmuch", "threads", 3,
                                 ret = 'focus')
        self.query_pane = p1
        p2 = p1.call("doc:attach", ret='focus')
        p3 = p2.call("doc:assign", p0, "notmuch:threads", ret='focus')
        self.query_pane = p3
        if num:
            self.query_pane.take_focus()
        self.resize()
        return 1

    def handle_select_message(self, key, focus, mark, num, str, str2, **a):
        "handle:notmuch:select-message"
        # a thread or message was selected. id in 'str'. threadid in str2
        # Find the file and display it in a 'message' pane
        self.mark_read()

        p0 = self.call("doc:notmuch:byid", str, ret='focus')
        p1 = self.query_pane.call("OtherPane", "notmuch", "message", 2,
                                  ret='focus')
        p2 = p1.call("doc:attach", ret='focus')
        p3 = p2.call("doc:assign", p0, "notmuch:message", ret='focus')

        # FIXME This still doesn't work: there are races: attaching a doc to
        # the pane causes the current doc to be closed.  But the new doc
        # hasn't been anchored yet so if they are the same, we lose.
        # Need a better way to anchor a document.
        #p0.call("doc:set:autoclose", 1)
        p = self.message_pane = p3
        p.ctid = str2
        p.cmid = str
        if num:
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
        edlib.Pane.__init__(self, focus)
        self['render-wrap'] = 'no'
        self['background'] = 'color:#A0FFFF'
        self['line-format'] = '<%fmt>%count %+name</>'
        self.call("Request:Notify:doc:Replace")
        self.selected = None

    def handle_clone(self, key, focus, mark, num, **a):
        "handle:Clone"
        p = notmuch_main_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_notify_replace(self, key, focus, mark, num, **a):
        "handle:Notify:doc:Replace"
        self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
        return 0

    def handle_select(self, key, focus, mark, num, **a):
        "handle:notmuch:select"
        s = focus.call("doc:get-attr", "query", mark, ret='str')
        if s:
            focus.call("notmuch:select-query", s, num)
        return 1

##################
# list-view shows a list of threads/messages that match a given
# search query.
# We generate the thread-ids using "notmuch search --output=threads"
# For a full-scan we collect at most 100 and at most 1 month at a time, until
# we reach an empty month, then get all the rest together
# For an update, we just check the last day and add anything missing.
# We keep an array of thread-ids
#
# Three different views are presented of this document by notmuch_query_view
# depending on whether 'selected' and possibly 'whole_thread' are set.
#
# By default when neither is set, only the threads are displayed.
# If a threadid is 'selected' but not 'whole_thread', then other threads
# appear as a single line, but the selected thread displays as all
# matching messages.
# If whole_thread is set, then only the selected thread is visible,
# and all messages, matched or not, of that thread are visisble.
#

class notmuch_list(edlib.Doc):
    def __init__(self, focus, qname, query):
        edlib.Doc.__init__(self, focus)
        self.db = notmuch_db()
        self.query = query
        self['qname'] = qname
        self['query'] = query
        self.threadids = []
        self.threads = {}
        self.messageids = {}
        self.threadinfo = {}
        self["render-default"] = "notmuch:query"
        self["line-format"] = "<%TM-hilite>%TM-date_relative</><tab:130></> <fg:blue>%+TM-authors</><tab:350>%.TM-threadinfo<tab:450><%TM-hilite>%.TM-subject</>                      "
        self.add_notify(self.parent, "Notify:Tag")
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
                    m.pos = (self.new[0], None)
                m = m.next_any()
        self.threadids = self.new + self.old
        self.notify("Notify:doc:Replace")
        if found < 100 and self.age == None:
            # must have found them all
            self.call("doc:notmuch:query-updated")
            return -1
        # request some more
        if found > 3:
            # allow for a little over-lap across successive calls
            self.offset += found - 3
        if found < 5:
            # stop worrying about age
            self.age = None
        if found < 100 and self.age:
            self.age += 1
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

    def load_thread(self, mark):
        (tid, mid) = mark.pos
        with self.db as db:
            q = notmuch.Query(db, "thread:%s and (%s)" % (tid, self.query))
            tl = list(q.search_threads())
            if not tl:
                q = notmuch.Query(db, "thread:%s" % (tid))
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
        if mid is None:
            # need to update all marks at this location to old mid
            m = mark
            pos = (tid, midlist[0])
            while m and m.pos and m.pos[0] == tid:
                m.pos = pos
                m = m.prev_any()
            m = mark.next_any()
            while m and m.pos and m.pos[0] == tid:
                m.pos = pos
                m = m.next_any()

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

    def cvt_depth(self, depth):
        ret = ""

        for level in depth[:-2]:
            ret += u" │ "[level]
        ret += u"╰├─"[depth[-2]]
        ret += u"─┬"[depth[-1]]

        return ret + "> "

    def step(self, mark, forward, move):
        ret = edlib.WEOF
        if forward:
            if mark.pos is None:
                return ret
            if not move:
                return '\n'
            (tid,mid) = mark.pos
            m2 = mark.next_any()
            while m2 and m2.pos == mark.pos:
                mark.to_mark(m2)
                m2 = mark.next_any()
            i = self.threadids.index(tid)
            if mid:
                j = self.messageids[tid].index(mid) + 1
            if mid and j < len(self.messageids[tid]):
                mark.pos = (tid, self.messageids[tid][j])
            elif i+1 < len(self.threadids):
                tid = self.threadids[i+1]
                if tid in self.messageids:
                    mark.pos = (tid, self.messageids[tid][0])
                else:
                    mark.pos = (tid, None)
            else:
                mark.pos = None
            m2 = mark.next_any()
            while m2 and m2.pos == mark.pos:
                mark.to_mark(m2)
                m2 = mark.next_any()
            return '\n'
        else:
            j = 0
            if mark.pos == None:
                i = len(self.threadids)
            else:
                (tid,mid) = mark.pos
                i = self.threadids.index(tid)
                if mid:
                    j = self.messageids[tid].index(mid)
            if i == 0 and j == 0:
                return edlib.WEOF
            if not move:
                return '\n'
            m2 = mark.prev_any()
            while m2 and m2.pos == mark.pos:
                mark.to_mark(m2)
                m2 = mark.prev_any()

            if j == 0:
                i -= 1
                tid = self.threadids[i]
                if tid in self.messageids:
                    j = len(self.messageids[tid])
                else:
                    j = 1
            j -= 1
            if tid in self.messageids:
                mark.pos = (tid, self.messageids[tid][j])
            else:
                mark.pos = (tid, None)

            m2 = mark.prev_any()
            while m2 and m2.pos == mark.pos:
                mark.to_mark(m2)
                m2 = mark.prev_any()
            return '\n'

    def handle_notoify_tag(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:Notify:Tag"
        if str2:
        # re-evaluate tags of a single message
            if str in self.threadinfo:
                t = self.threadinfo[str]
                if str2 in t:
                    tg = t[str2][6]
                    s = self.call("doc:notmuch:byid:tags", str2, ret='str')
                    tg[:] = s.split(",")

        if str in self.threads:
            t = self.threads[str]
            s = self.call("doc:notmuch:bythread:tags", str, ret='str')
            t['tags'] = s.split(",")
        self.notify("Notify:doc:Replace")
        return 1

    def handle_set_ref(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:set-ref"
        mark.pos = None
        if num == 1 and len(self.threadids) > 0:
            tid = self.threadids[0]
            if tid in self.messageids:
                mark.pos = (self.threadids[0],self.messageids[tid][0])
            else:
                mark.pos = (self.threadids[0],None)
        mark.offset = 0
        self.to_end(mark, num == 0)
        return 1

    def handle_step(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:step"
        forward = num
        move = num2
        return self.step(mark, forward, move)

    def handle_step_thread(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:step-thread"
        # Move to the start of the current thread, or the start
        # of the next one.
        forward = num
        move = num2
        if forward:
            if mark.pos == None:
                return edlib.WEOF
            if not move:
                return "\n"
            (tid,mid) = mark.pos
            m2 = mark.next_any()
            while m2 and m2.pos != None and m2.pos[0] == tid:
                mark.to_mark(m2)
                m2 = mark.next_any()
            i = self.threadids.index(tid) + 1
            if i < len(self.threadids):
                tid = self.threadids[i]
                if tid in self.messageids:
                    mark.pos = (tid, self.messageids[tid][0])
                else:
                    mark.pos = (tid, None)
            else:
                mark.pos = None
            return '\n'
        else:
            if mark.pos == None:
                if len(self.threadids) == 0:
                    return edlib.WEOF
                tid = self.threadids[-1]
            else:
                (tid,mid) = mark.pos
            m2 = mark.prev_any()
            while m2 and (m2.pos == None or m2.pos[0] == tid):
                mark.to_mark(m2)
                m2 = mark.prev_any()
            if tid in self.messageids:
                mark.pos = (tid, self.messageids[tid][0])
            else:
                mark.pos = (tid, None)
            return '\n'

    def handle_step_matched(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:step-matched"
        # Move to the next/prev message which is matched.
        forward = num
        move = num2
        m = mark
        if not move:
            m = mark.dup()
        ret = self.step(m, forward, 1)
        while ret != edlib.WEOF and m.pos != None:
            (tid,mid) = m.pos
            if not mid:
                break
            ms = self.threadinfo[tid][mid]
            if ms[2]:
                break
            ret = self.step(m, forward, 1)
        return ret

    def handle_doc_get_attr(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:get-attr"
        attr = str
        if mark.pos == None:
            # No attributes for EOF
            return 1
        (tid,mid) = mark.pos
        i = self.threadids.index(tid)
        j = 0;
        if mid:
            j = self.messageids[tid].index(mid)

        val = None

        tid = self.threadids[i]
        t = self.threads[tid]
        if mid:
            m = self.threadinfo[tid][mid]
            (fn, dt, matched, depth, author, subj, tags) = m
        else:
            (fn, dt, matched, depth, author, subj, tags) = (
                "", 0, False, [0,0], "" ,"", [])
        if attr == "message-id":
            val = mid
        elif attr == "thread-id":
            val = tid
        elif attr == "T-hilite":
            if "inbox" not in t["tags"]:
                # FIXME this test is wrong once we have generic searches
                val = "fg:grey"
            elif "new" in t["tags"] and "unread" in t["tags"]:
                val = "fg:red,bold"
            elif "unread" in t["tags"]:
                val = "fg:blue"
            else:
                val = "fg:black"
        elif attr == "T-date_relative":
            val = self.rel_date(t['timestamp'])
        elif attr == "T-threadinfo":
            val = "[%d/%d]" % (t['matched'],t['total'])
        elif attr[:2] == "T-" and attr[2:] in t:
            val = t[attr[2:]]
            if type(val) == int:
                val = "%d" % val
            else:
                val = unicode(t[attr[2:]])
            if attr == "T-authors":
                val = val[:20]

        elif attr == "message-id":
            val = mid
        elif attr == "thread-id":
            val = tid
        elif attr == "matched":
            val = "True" if matched else "False"
        elif attr == "M-hilite":
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
        elif attr == "M-date_relative":
            val = self.rel_date(dt)
        elif attr == "M-authors":
            val = author[:20]
        elif attr == "M-subject":
            val = subj
        elif attr == "M-threadinfo":
            val = self.cvt_depth(depth)

        if not val is None:
            comm2("callback", focus, val)
        return 1

    def handle_get_attr(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:get-attr" and comm2
        if str == "doc-type":
            comm2("callback", focus, "notmuch-list")
            return 1
        return 0

    def handle_load_thread(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:notmuch:load-thread"
        if mark.pos is None:
            return -1
        (tid,mid) = mark.pos
        if tid not in self.threadinfo:
            self.load_thread(mark)
        if tid in self.threadinfo:
            return 1
        return 2

    def handle_same_search(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:notmuch:same-search"
        if self.query == str2:
            return 1
        return 2

    def handle_query_refresh(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:notmuch:query-refresh"
        self.load_update()

    def handle_mark_read(self, key, mark, mark2, num, num2, focus, xy, str, str2, comm2, **a):
        "handle:doc:notmuch:mark-read"
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
        self.notify("Notify:doc:Replace")
        # Let this fall though to database document.
        return 0

class notmuch_query_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.selected = None
        self.whole_thread = 0
        self.seen_threads = {}
        self.seen_msgs = {}
        # thread_start and thread_end are marks which deliniate
        # the 'current' thread. thread_matched is the first matched
        # message in the thread.
        self.thread_start = None
        self.thread_end = None
        self.thread_matched = None
        self.call("Request:Notify:doc:Replace")

    def handle_clone(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:Clone"
        p = notmuch_query_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_notify_replace(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:Notify:doc:Replace"
        self.damaged(edlib.DAMAGED_CONTENT)
        return 1

    def handle_set_ref(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:doc:set-ref"
        start = num
        if start:
            if self.whole_thread:
                mark.to_mark(self.thread_start)
                return 1
            if self.thread_matched and self.parent.call("doc:step", 0, mark) == edlib.WEOF:
                # first thread is open
                mark.to_mark(self.thread_matched)
                return 1;
        # otherwise fall-through to real start or end
        return 0

    def handle_step(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:doc:step"
        forward = num
        move = num2
        if self.whole_thread:
            # move one message, but stop at thread_start/thread_end
            if forward:
                ret = self.parent.call("doc:step", focus, forward, move, mark)
                if move and mark >= self.thread_end:
                    # If at end of thread, move to end of document
                    self.parent.call("doc:set-ref", focus, mark, 0)
            else:
                if mark <= self.thread_start:
                    # at start already
                    ret = edlib.WEOF
                else:
                    ret = self.parent.call("doc:step", focus, forward, move, mark)
            return ret
        else:
            # if between thread_start/thread_end, move one message,
            # else move one thread
            if not self.thread_start:
                in_thread = False
            elif forward and mark >= self.thread_end:
                in_thread = False
            elif not forward and mark <= self.thread_matched:
                in_thread = False
            elif forward and mark < self.thread_matched:
                in_thread = False
            elif not forward and mark > self.thread_end:
                in_thread = False
            else:
                in_thread = True
            if in_thread:
                # move one matched message
                ret = self.parent.call("doc:step-matched", focus, mark, forward, move)
                # If moving forward, we might be in the next thread,
                # make sure we didn't go further than thread_end
                if forward and move and mark.seq > self.thread_end.seq:
                    mark.to_mark(self.thread_end)
                return ret
            else:
                # move one thread
                if forward:
                    ret = self.parent.call("doc:step-thread", focus, mark, forward, move)
                    if self.thread_start and mark == self.thread_start:
                        mark.to_mark(self.thread_matched)
                else:
                    ret = self.parent.call("doc:step", focus, mark, forward, move)
                    if move and ret != edlib.WEOF:
                        self.parent.call("doc:step-thread", focus, mark, forward, move)
                return ret

    def handle_get_attr(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:doc:get-attr"
        if mark is None:
            mark = self.call("doc:point", ret='mark')
        attr = str
        if attr[:3] == "TM-":
            if self.thread_start and mark < self.thread_end and mark >= self.thread_start:
                return self.parent.call("doc:get-attr", focus, num, num2, mark, "M-" + str[3:], comm2)
            else:
                return self.parent.call("doc:get-attr", focus, num, num2, mark, "T-" + str[3:], comm2)
        return 0

    def handle_Z(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:Chr-Z"
        if not self.thread_start:
            return 1
        if self.whole_thread:
            # all non-match messages in this thread are about to
            # disappear, we need to clip them.
            mk = self.thread_start.dup()
            mt = self.thread_matched.dup()
            while mk.seq < self.thread_end.seq:
                if mk.seq < mt.seq:
                    focus.call("Notify:clip", mk, mt)
                mk.to_mark(mt)
                self.parent.call("doc:step-matched", mt, 1, 1)
                self.parent.call("doc:step", mk, 1, 1)
        else:
            # everything before the read, and after the thread disappears
            m = edlib.Mark(self)
            focus.call("Notify:clip", m, self.thread_start)
            self.call("doc:set-ref", m, 0)
            focus.call("Notify:clip", self.thread_end, m)
        self.whole_thread = 1 - self.whole_thread
        # notify that everything is changed, don't worry about details.
        focus.call("Notify:change")
        return 1

    def handle_select(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:notmuch:select"
        s = focus.call("doc:get-attr", "thread-id", mark, ret='str')
        if s != self.selected:
            if self.selected:
                # old thread is disappearing.
                focus.call("Notify:clip", self.thread_start, self.thread_end)
                focus.call("Notify:change", self.thread_start, self.thread_end)
                self.selected = None
                self.thread_start = None
                self.thread_end = None
                self.thread_matched = None

            if self.call("doc:notmuch:load-thread", mark) == 1:
                self.selected = s
                if mark:
                    self.thread_start = mark.dup()
                else:
                    self.thread_start = focus.call("doc:dup-point", 0, -2, ret='mark')
                self.parent.call("doc:step-thread", 0, 1, self.thread_start)
                self.thread_end = self.thread_start.dup()
                self.parent.call("doc:step-thread", 1, 1, self.thread_end)
                self.thread_matched = self.thread_start.dup()
                if self.parent.call("doc:get-attr", self.thread_matched, "matched", ret="str") != "True":
                    self.parent.call("doc:step-matched", 1, 1, self.thread_matched)
                focus.call("Notify:change", self.thread_start, self.thread_end)
        s2 = focus.call("doc:get-attr", "message-id", mark, ret='str')
        if s2 and num >= 0:
            focus.call("notmuch:select-message", s2, s, num)
        return 1

    def handle_reposition(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:render:reposition"
        # some messages have been displayed, from mark to mark2
        # collect threadids and message ids
        if not mark or not mark2:
            return 0
        m = mark.dup()

        while m.seq < mark2.seq:
            i1 = focus.call("doc:get-attr", "thread-id", m, ret='str')
            i2 = focus.call("doc:get-attr", "message-id", m, ret='str')
            if i1 and not i2 and i1 not in self.seen_threads:
                self.seen_threads[i1] = True
            if i1 and i2:
                if i1 in self.seen_threads:
                    del self.seen_threads[i1]
                if  i2 not in self.seen_msgs:
                    self.seen_msgs[i2] = True
            if edlib.WEOF == focus.call("doc:step", 1, 1, m):
                break

    def handle_mark_seen(self, key, focus, mark, mark2, num, num2, str, comm2, **a):
        "handle:doc:notmuch:mark-seen"
        for i in self.seen_threads:
            self.parent.call("doc:notmuch:remember-seen-thread", i)
        for i in self.seen_msgs:
            self.parent.call("doc:notmuch:remember-seen-msg", i)
        self.parent.call("doc:notmuch:mark-seen")
        return 1

class notmuch_message_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        # Need to set default visibility on each part.
        # step forward with doc:step-part and for any 'odd' part,
        # which is a spacer, we look at email:path and email:content-type.
        # If alternative:[1-9] is found, or type isn't "text*", make it
        # invisible.
        p = 0
        m = edlib.Mark(focus)
        while True:
            newp = self.call("doc:step-part", m, 1)
            # retval is offset by 1 to avoid zero
            newp -= 1
            if newp <= p:
                # reached the end
                break
            p = newp
            if not (p & 1):
                continue
            if p < 2:
                continue
            path = focus.call("doc:get-attr", "multipart-prev:email:path", m, ret='str')
            type = focus.call("doc:get-attr", "multipart-prev:email:content-type", m, ret='str')
            vis = True
            for  el in path.split('.'):
                if el[:12] == "alternative:" and el != "alternative:0":
                    vis = False
            if type[:4] != "text" and type != "":
                vis = False
            if not vis:
                focus.call("doc:set-attr", "email:visible", m, 0)

    def handle_close(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Close"
        self.call("notmuch-close-message")
        return 1

    def handle_clone(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Clone"
        p = notmuch_message_view(focus)
        self.clone_children(focus.focus)
        return 1

    def handle_replace(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Replace"
        return 1

    def handle_slash(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Chr-/"
        s = focus.call("doc:get-attr", mark, "email:visible", ret='str')
        if not s:
            return 1
        if s == "0":
            focus.call("doc:set-attr", mark, "email:visible", "1")
        else:
            focus.call("doc:set-attr", mark, "email:visible", "0")
        return 1

    def handle_space(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Chr- "
        if focus.call("Next", 1, mark) == 2:
            focus.call("Chr-n", mark)
        return 1

    def handle_backspace(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Backspace"
        if focus.call("Prior", 1, mark) == 2:
            focus.call("Chr-p", mark)
        return 1

    def handle_return(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Return"
        focus.call("doc:email:select", mark);
        return 1

    def handle_activate(self, key, focus, mark, num, str, str2, comm2, **a):
        "handle:Mouse-Activate"
        focus.call("doc:email:select", mark);
        return 1

    def handle_map_attr(self, key, focus, mark, num, str, str2, comm2, **a):
        if str == "render:rfc822header":
            comm2("attr:callback", focus, int(str2), mark, "fg:#6495ed", 21)
            comm2("attr:callback", focus, 10000, mark, "wrap-tail: ,wrap-head:    ", 19)
            return 1
        if str == "render:rfc822header-wrap":
            comm2("attr:callback", focus, int(str2), mark, "wrap", 20)
            return 1
        if str == "render:rfc822header-subject":
            comm2("attr:callback", focus, int(str2), mark, "fg:blue,bold", 20)
            return 1
        if str == "render:rfc822header-to":
            comm2("attr:callback", focus, int(str2), mark, "fg:blue,bold", 20)
            return 1

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

def render_query_attach(key, home, focus, comm2, **a):
    p = focus.render_attach("format")
    p = notmuch_query_view(p)
    if comm2:
        comm2("callback", p)
    return 1

def render_message_attach(key, home, focus, comm2, **a):
    p = focus.render_attach("default:email-view")
    p = notmuch_message_view(p)
    if comm2:
        comm2("callback", p)
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
    p0 = focus.call("ThisPane", ret = 'focus')
    try:
        p1 = home.call("docs:byname", "*Notmuch*", ret='focus')
    except:
        p1 = home.call("attach-doc-notmuch", ret='focus')
    if not p1:
        return -1
    p2 = p0.call("doc:attach", ret = 'focus')
    p2.call("doc:assign", p1, 1)
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
