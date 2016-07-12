
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

db = None

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
        self.update_one()

    def update_one(self):
        if not self.todo:
            self.pane = None
            self.cb = None
            return
        n = self.todo[0]
        self.p = Popen("/usr/bin/notmuch count --batch", shell=True, stdin=PIPE,
                       stdout = PIPE)
        self.p.stdin.write(self.make_search(n, False) + "\n")
        self.p.stdin.write(self.make_search(n, 'unread') + "\n")
        self.p.stdin.write(self.make_search(n, 'new') + "\n")
        self.p.stdin.close()
        self.pane.call("event:read", self.p.stdout.fileno(), self.cb)

    def updated(self, *a):
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
        self.update_one()
        p.wait()

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

    def handle(self, key, focus, mark, mark2, numeric, extra, str, comm2, **a):
        if key == "doc:set-ref":
            if numeric == 1:
                mark.offset = 0
            else:
                mark.offset = len(self.searches.current)
            mark.rpos = 0
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
                if attr == 'fmt':
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

        if key == "notmuch:update":
            if not self.timer_set:
                self.timer_set = True
                self.call("event:timer", 60*5, self.tick)
            self.searches.load(False)
            self.notify("Notify:Replace")
            self.searches.update(self, self.updated)
            return 1

        if key == "notmuch-follow":
            if mark.offset < len(self.searches.current):
                pl = []
                s = self.searches.current[mark.offset]
                s2 = self.searches.make_search(s, None)
                root = self
                while root.parent:
                    root = root.parent
                root.call("attach-doc-notmuch-list", s2, s, comm2)

                return 1
        if key == "notmuch-search-maxlen":
            return self.searches.maxlen + 1

    def tick(self, key, **a):
        if not self.searches.todo:
            self.searches.load(False)
            self.searches.update(self, self.updated)
        return 1

    def updated(self, key, **a):
        self.searches.updated()
        self.notify("Notify:Replace")
        return -1

def notmuch_doc(key, home, focus, comm2, **a):
    # Create the root notmuch document
    nm = notmuch_main(home)
    nm['render-default'] = "notmuch:searchlist"
    nm.call("doc:set-name", "*Notmuch*")
    nm.call("global-multicall-doc:appeared-")
    nm.call("notmuch:update")
    if comm2 is not None:
        comm2("callback", focus, nm)
    return 1

class notmuch_main_view(edlib.Pane):
    # This pane provides view on the search-list document.
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)
        self['render-wrap'] = 'no'
        self['background'] = 'color:#A0FFFF'
        self['line-format'] = '<%fmt>%count %+name</>'
        self.call("Request:Notify:Replace")
        self.maxlen = 0

    def handle(self, key, focus, mark, numeric, **a):
        if key == "Clone":
            p = notmuch_main_view(focus)
            self.clone_children(focus.focus)
            return 1
        if key == "Chr-g":
            focus.call("notmuch:update")
            self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
            return 1
        if key == "Notify:Replace":
            self.damaged(edlib.DAMAGED_CONTENT|edlib.DAMAGED_VIEW)
            return 0

        if key == "Refresh:size":
            pl = []
            focus.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) == 2:
                if pl[0].w < pl[1].w:
                    # we have split, make sure proportions are OK
                    (s,s2,side,type) = self.list_info(pl[1])
                    if pl[0].w < s:
                        focus.call("Window:x+", "notmuch", s - pl[0].w)
                    elif pl[0].w > s:
                        focus.call("Window:x-", "notmuch", pl[0].w - s)
            return 0
        return notmuch_handle(self, key, focus, numeric, mark)

    def list_info(self,pane):
        if self.maxlen <= 0:
            self.maxlen = self.call("notmuch-search-maxlen")
            if self.maxlen > 1:
                self.maxlen -= 1

        space = pane.w
        ch,ln = self.scale()
        max = 5 + 1 + self.maxlen + 1
        if space * 100 / ch < max * 4:
            w = space / 4
        else:
            w = ch * 10 * max / 1000
        return (w, space-w, 3, "threads")


def render_searchlist_attach(key, focus, comm2, **a):
    # A searchlist is rendered inside a tiler so that sub-windows
    # created from it stay together in the tiler.  That means we need
    # to add a local 'view'
    pl = []
    focus.call("attach-tile", "notmuch", "main", lambda key,**a:take('focus',pl,a))
    p = pl[-1]
    p.call("attach-view", lambda key,**a:take('focus',pl,a))
    p = pl[-1]
    p = p.render_attach("format")
    p = notmuch_main_view(p)
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
    pl[1].call("doc:attach", pl[0])
    return 1

##################
# list-view shows a list of threads/messages that match a given
# search query.
# We generate the thread-ids using "notmuch search --output=threads"
# For a full-scan we collect at most 100 and at most 1 month at a time, until
# we reach and empty month, then get all the rest together
# For an update, we just check the last day and add anything missing.
# We keep an array of thread-ids

class notmuch_list(edlib.Doc):
    def __init__(self, focus, query):
        edlib.Doc.__init__(self, focus, self.handle)
        self.query = query
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
        self.start_load()

    def start_load(self):
        cmd = ["/usr/bin/notmuch", "search", "--output=summary", "--format=json", "--limit=100", "--offset=%d" % self.offset ]
        if self.age:
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
                if i > 0:
                    self.move_marks_from(tid)
                del self.old[i]
            except ValueError:
                pass
            if tid not in self.new:
                self.new.append(tid)
            self.threads[tid] = j
        tl = None
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
            return -1
        # request some more
        if found < 5:
            # stop worrying about age
            self.age = None
        if found < 100 and self.age:
            self.age += 1
        # allow for a little over-lap across successive calls
        self.offset += found - 3
        self.start_load()
        return -1

    def add_message(self, m, lst, info, depth):
        mid = m.get_message_id()
        lst.append(mid)
        info[mid] = (m.get_filename(), m.get_date(),
                     m.get_flag(notmuch.Message.FLAG.MATCH),
                     depth, m.get_header("From"), m.get_header("Subject"), m.get_tags())
        l = list(m.get_replies())
        if l:
            l.sort(key=lambda m:(m.get_date(), m.get_header("subject")))
            for m in l[:-1]:
                self.add_message(m, lst, info, depth * 2 + 1)
            self.add_message(l[-1], lst, info, depth * 2)

    def _load_thread(self, tid):
        global db
        if not db:
            db = notmuch.Database()
        q = notmuch.Query(db, "thread:%s and (%s)" % (tid, self.query))
        thread = list(q.search_threads())[0]
        midlist = []
        minfo = {}
        ml = list(thread.get_toplevel_messages())
        ml.sort(key=lambda m:(m.get_date(), m.get_header("subject")))
        for m in ml[:-1]:
            self.add_message(m, midlist, minfo, 3)
        self.add_message(ml[-1], midlist, minfo, 2)
        self.messageids[tid] = midlist
        self.threadinfo[tid] = minfo

    def load_thread(self, tid):
        global db
        aborted = True
        while aborted:
            try:
                self._load_thread(tid)
                aborted = False
            except notmuch.XapianError:
                db.close()
                db = None

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

    def next(self, pos, visible):
        # visible is a list of visible thread-ids
        # we move pos forward either to the next message in a visible thread
        # or the first message of the next thread
        # return index into thread list and index into message list of original pos,
        # and new pos
        if pos is None:
            return (-1,-1,None)
        th = pos[0]
        i = self.threadids.index(th)
        j = -1
        if th in visible:
            # maybe step to next message
            if len(pos) == 1:
                j = 0
            else:
                j = self.messageids[th].index(pos[1])
            if j <= len(messageids[th]):
                return (i, j, (th, self.messageids[j+1]))
        # need next thread
        if i+1 >= len(self.threadids):
            return (i-1, j, None)
        th = self.threadids[i+1]
        if th in visible:
            ms = self.messageids[th][0]
            return (i, j, (th, ms))
        return (i, -1, (th,))

    def prev(self, pos, visible):
        # visible is a list of visible thread-ids
        # we move pos backward either to previous next message in a visible thread
        # or the last message of the previous thread
        # return index into thread list, index into message list, and new pos
        # (Indexs are new pos!)
        if pos is not None and pos[0] in visible and len(pos) == 2:
            # maybe go to previous message
            th = pos[0]
            i = self.threadids.index(th)
            ms = pos[1]
            j = self.messageids[th].index(ms)
            if j > 0:
                j -= 1
                return (i,j,(th, self.messageids[th][j]))
        # need previous thread
        if pos is None:
            i = len(self.threadids)
        else:
            i = self.threadids.index(pos[0])
        if i == 0:
            return (-1, -1, None)
        i -= 1
        th = self.threadids[i]
        if th not in visible:
            return (i, -1, (th,))
        j = len(self.messageids[th])
        ms = self.messageids[th][j-1]
        return (i, j, (th, ms))

    def cvt_depth(self, depth):
        ret = ""
        while depth > 1:
            if depth & 1:
                ret = "+" + ret
            else:
                ret = "-" + ret

            depth = int(depth/2)
        return ret + "> "

    def handle(self, key, mark, mark2, numeric, extra, focus, str, str2, comm2, **a):
        if key == "doc:set-ref":
            if numeric == 1 and len(self.threadids) > 0:
                mark.pos = (self.threadids[0],)
            else:
                mark.pos = None
            mark.offset = 0
            mark.rpos = 0
            return 1

        if key == "doc:mark-same":
            if mark.pos == mark2.pos:
                return 1
            if mark.pos is None or mark2.pos is None or mark.pos[0] != mark2.pos[0]:
                # definitely different
                return 2
            # same thread, possible different messages
            if mark.pos != str2:
                # thread not open, so same
                return 1
            if len(mark.pos) == 1:
                m = mark2.pos[1]
            elif len(mark2.pos) == 1:
                m = mark.pos[1]
            else:
                return 2
            # one has no message, the other has one. so same if
            # that one is the first.
            if self.messageids[mark.pos[0]][0] == m:
                return 1
            return 2

        if key == "doc:step":
            forward = numeric
            move = extra
            ret = edlib.WEOF
            if mark.pos == None:
                i = len(self.threadids)
            else:
                i = self.threadids.index(mark.pos[0])
            if forward:
                i2,j2,pos = self.next(mark.pos, [str2])
                if mark.pos is not None:
                    ret = ' '
                if move:
                    m2 = mark.next_any()
                    target = None
                    while m2 and m2.pos == mark.pos:
                        target = m2
                        m2 = m2.next_any()
                    if target:
                        mark.to_mark(target)
                    mark.pos = pos
            if not forward:
                i2,j2,pos = self.prev(mark.pos, [str2])
                if i2 >= 0:
                    ret = ' '
                    if move:
                        m2 = mark.prev_any()
                        target = None
                        while m2 and m2.pos == mark.pos:
                            target = m2
                            m2 = m2.prev_any()
                        if target:
                            mark.to_mark(target)
                        mark.pos = pos
            return ret

        if key == "doc:get-attr":
            attr = str
            forward = numeric
            if forward:
                i,j,newpos = self.next(mark.pos, [str2])
            else:
                i,j,newpos = self.prev(mark.pos, [str2])

            val = "["+attr+"]"
            if i >= 0 and j == -1:
                # report on thread, not message
                tid = self.threadids[i]
                t = self.threads[tid]
                if attr == "hilite":
                    if "new" in t["tags"] and "unread" in t["tags"]:
                        val = "fg:red,bold"
                    elif "unread" in t["tags"]:
                        val = "fg:blue"
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

            if j >= 0:
                # report on an individual message
                tid = self.threadids[i]
                mid = self.messageids[tid][j]
                m = self.threadinfo[tid][mid]
                (fn, dt, matched, depth, author, subj, tags) = m
                if attr == "hilite":
                    if not matched:
                        val = "fg:grey"
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

        if key == "notmuch-follow":
            if mark.pos is None:
                return 1
            if mark.pos[0] not in self.threadinfo:
                self.load_thread(mark.pos[0])
            th = self.threads[mark.pos[0]]
            id = th['query'][0].split()[0]
            global db
            if not db:
                db = notmuch.Database()
            try:
                m = db.find_message(id[3:])
            except:
                db = notmuch.Database()
                m = db.find_message(id[3:])
            fn = m.get_filename()
            try:
                f = open(fn)
            except:
                focus.call("Message", "Cannot open " + fn)
                return 1
            pl = []
            focus.call("doc:open", fn, f.fileno(), lambda key,**a:take('focus',pl,a))
            f.close()
            if pl:
                comm2("callback", pl[0], mark.pos[0])
            return 1

class notmuch_query_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)

    def handle(self, key, focus, mark, numeric, **a):
        if key == "Clone":
            p = notmuch_query_view(focus)
            self.clone_children(focus.focus)
            return 1

        if key == "Refresh:size":
            pl = []
            focus.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) == 2:
                if pl[0].h < pl[1].h:
                    # we have split, make sure proportions are OK
                    (s, s2, size, type) = self.list_info(pl[1])
                    if pl[0].h != s:
                        focus.damaged(edlib.DAMAGED_VIEW)
            return 0
        if key == "Refresh:view":
            # check size
            pl = []
            focus.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) == 2:
                if pl[0].h < pl[1].h:
                    # we have split, make sure proportions are OK
                    (s,s2, size, type) = self.list_info(pl[1])
                    if pl[0].h < s:
                        focus.call("Window:y+", "notmuch", s - pl[0].h)
                    elif pl[0].h > s:
                        focus.call("Window:y-", "notmuch", pl[0].h - s)
            return 0
        return notmuch_handle(self, key, focus, numeric, mark)

    def list_info(self,pane):
        ch,ln = self.scale()
        space = pane.h
        min = 4
        if space * 100 / ln > min * 4:
            h = space / 4
        else:
            h = ln * 10 * min / 1000
            if h > space / 2:
                h = space / 2
        return (h, space - h, 2, "message")

class notmuch_message_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)

    def handle(self, key, focus, mark, numeric, **a):
        if key == "Clone":
            p = notmuch_message_view(focus)
            self.clone_children(focus.focus)
            return 1
        if key == "notmuch-follow":
            return 1
        if key == "Replace":
            return 1
        return notmuch_handle(self, key, focus, numeric, mark)

def notmuch_handle(pane, key, focus, numeric, mark):
    # common handler for all sub-panes
    if key == "Chr-o":
        # focus to next window
        focus.call("Window:next", "notmuch", numeric)
        return 1

    if key == "Return":
        pl = []
        focus.call("notmuch-follow", mark, lambda key,**a:take('focus',pl,a))
        if not pl:
            return 1
        focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
        if len(pl) != 2:
            return 1
        (size1, size2, side, type) = pane.list_info(pl[1])
        focus.call("OtherPane", "notmuch", type, side, size2, lambda key,**a:take('focus', pl, a))
        pl[0].call('doc:attach', pl[-1], "notmuch:"+type, lambda key,**a:take('focus', pl, a))
        #pl[-1].call("doc:autoclose", 1)
        return 1

    if key == "Chr- ":
        # "Space", like return but change focus, or "page-down", or "next"...
        pl = []
        focus.call("notmuch-follow", mark, lambda key,**a:take('focus',pl,a))
        if not pl:
            focus.call("Next", 1, mark)
            # FIXME detect EOF and move to next message
            return 1
        focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
        if len(pl) != 2:
            return 1
        (size1, size2, side, type) = pane.list_info(pl[1])
        focus.call("OtherPane", "notmuch", type, side, size2, lambda key,**a:take('focus', pl, a))
        pl[0].call('doc:attach', pl[-1], "notmuch:"+type, lambda key,**a:take('focus', pl, a))
        pl[-1].take_focus()
        #pl[-1].call("doc:autoclose", 1)
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

def notmuch_open_list(key, home, focus, str, str2, comm2, **a):
    nm = notmuch_list(home, str)
    if str2 is not None:
        s = str2
    else:
        s = str
    nm.call("doc:set-name", s)
    nm.call("global-multicall-doc:appeared-")
    if comm2 is not None:
        comm2("callback", focus, nm)

if "editor" in globals():
    editor.call("global-set-command", "attach-doc-notmuch", notmuch_doc)
    editor.call("global-set-command", "attach-render-notmuch:searchlist",
                render_searchlist_attach)
    editor.call("global-set-command", "attach-doc-notmuch-list", notmuch_open_list)
    editor.call("global-set-command", "attach-render-notmuch:threads",
                render_query_attach)
    editor.call("global-set-command", "attach-render-notmuch:message",
                render_message_attach)
    editor.call("global-set-command", "interactive-cmd-nm", notmuch_mode)
