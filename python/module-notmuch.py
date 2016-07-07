
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
                if attr == 'namefmt':
                    if self.searches.new[s]:
                        val = "bold,fg:red"
                    elif self.searches.unread[s]:
                        val = "bold"
                    else:
                        val = ""
                elif attr == 'unreadfmt':
                    if self.searches.unread[s]:
                        val = "bold"
                    else:
                        val = ""
                elif attr == 'name':
                        val = "%-12s" % s
                elif attr == 'count':
                    if self.searches.count[s] is None:
                        val = "%6s" % "?"
                    else:
                        val = "%6d" % self.searches.count[s]
                elif attr == 'unread':
                    if self.searches.unread[s] is None:
                        val = "%6s" % "?"
                    else:
                        val = "%6d" % self.searches.unread[s]
                elif attr == 'new':
                    if self.searches.new[s] is None:
                        val = "%6s" % "?"
                    else:
                        val = "%6d" % self.searches.new[s]
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

        if key == "notmuch-show-list":
            if mark.offset < len(self.searches.current):
                pl = []
                s = self.searches.current[mark.offset]
                s2 = self.searches.make_search(s, None)
                root = self
                while root.parent:
                    root = root.parent
                root.call("attach-doc-notmuch-list", s2, s, comm2)

                return 1

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
        self['line-format'] = '<%namefmt>%+name</> %new <%unreadfmt>%unread</> %count'
        self.call("Request:Notify:Replace")

    def handle(self, key, focus, mark, **a):
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

        if key == "Return":
            pl = []
            focus.call("notmuch-show-list", mark, lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) != 2:
                return 1
            s = self.list_size(pl[1].w)
            focus.call("OtherPane", "notmuch", "threads", 3, focus.w - s, lambda key,**a:take('focus', pl, a))
            pl[0].call('doc:attach', pl[-1],  lambda key,**a:take('focus', pl, a))
            #pl[-1].call("doc:autoclose", 1)

        if key == "Refresh:size":
            pl = []
            focus.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) == 2:
                if pl[0].w < pl[1].w:
                    # we have split, make sure proportions are OK
                    s = self.list_size(pl[1].w)
                    if pl[0].w < s:
                        focus.call("Window:x+", "notmuch", s - pl[0].w)
                    elif pl[0].w > s:
                        focus.call("Window:x-", "notmuch", pl[0].w - s)
            return 0
    def list_size(self,space):
        ch,ln = self.scale()
        max = 34
        if space * 100 / ch < max * 4:
            w = space / 4
        else:
            w = ch * 10 * max / 1000
        return w


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
        self["render-default"] = "notmuch:query"
        self["line-format"] = "%date_relative<tab:130></> <fg:blue>%+authors</><tab:350> [%matched/%total] %subject                      "
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
        if not self.threadids:
            # first insertion, all marks must be at start
            m = self.first_mark()
            while m:
                if m.pos is None:
                    m.pos = self.new[0]
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

    def handle(self, key, mark, mark2, numeric, extra, focus, str, comm2, **a):
        if key == "doc:set-ref":
            if numeric == 1 and len(self.threadids) > 0:
                mark.pos = self.threadids[0]
            else:
                mark.pos = None
            mark.offset = 0
            mark.rpos = 0
            return 1

        if key == "doc:mark-same":
            return 1 if mark.pos == mark2.pos else 2

        if key == "doc:step":
            forward = numeric
            move = extra
            ret = edlib.WEOF
            if mark.pos == None:
                i = len(self.threadids)
            else:
                i = self.threadids.index(mark.pos)
            if forward and i < len(self.threadids):
                ret = ' '
                if move:
                    m2 = mark.next_any()
                    target = None
                    while m2 and m2.pos == mark.pos:
                        target = m2
                        m2 = m2.next_any()
                    if target:
                        mark.to_mark(target)
                    if i+1 < len(self.threadids):
                        mark.pos = self.threadids[i+1]
                    else:
                        mark.pos = None
            if not forward and i > 0:
                ret = ' '
                if move:
                    m2 = mark.prev_any()
                    target = None
                    while m2 and m2.pos == mark.pos:
                        target = m2
                        m2 = m2.prev_any()
                    if target:
                        mark.to_mark(target)
                    mark.pos = self.threadids[i-1]
            return ret

        if key == "doc:get-attr":
            attr = str
            forward = numeric
            if mark.pos == None:
                i = len(self.threadids)
            else:
                i = self.threadids.index(mark.pos)
            if not forward:
                i -= 1
            val = "["+attr+"]"
            if i >= 0 and i < len(self.threadids):
                tid = self.threadids[i]
                t = self.threads[tid]
                if attr in t:
                    val = t[attr]
                    if type(val) == int:
                        val = "%3d" % val
                    else:
                        val = unicode(t[attr])
                    if attr == 'date_relative':
                        val = "           " + val
                        val = val[-13:]
                    if attr == "authors":
                        val = val[:20]
            comm2("callback", focus, val)
            return 1

        if key == "notmuch-show-thread":
            if mark.pos is None:
                return 1
            th = self.threads[mark.pos]
            id = th['query'][0].split()[0]
            global db
            if not db:
                db = notmuch.Database()
            try:
                m = db.find_message(id[3:])
            except:
                print "try again"
                db = notmuch.Database()
                m = db.find_message(id[3:])
            fn = m.get_filename()
            try:
                f = open(fn)
            except:
                focus.call("Message", "Cannot open " + fn)
                return 1
            focus.call("doc:open", fn, f.fileno(), comm2)
            f.close()
            return 1

class notmuch_query_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus, self.handle)

    def handle(self, key, focus, mark, **a):
        if key == "Clone":
            p = notmuch_query_view(focus)
            self.clone_children(focus.focus)
            return 1

        if key == "Return":
            pl = []
            focus.call("notmuch-show-thread", mark, lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) != 2:
                return 1
            s = self.list_size(pl[1].h)
            focus.call("OtherPane", "notmuch", "message", 2, focus.h - s, lambda key,**a:take('focus', pl, a))
            pl[0].call('doc:attach', pl[-1],  lambda key,**a:take('focus', pl, a))
            #pl[-1].call("doc:autoclose", 1)

        if key == "Refresh:size":
            pl = []
            focus.call("ThisPane", "notmuch", lambda key,**a:take('focus',pl,a))
            focus.call("RootPane", "notmuch", lambda key,**a:take('focus',pl,a))
            if len(pl) == 2:
                if pl[0].h < pl[1].h:
                    # we have split, make sure proportions are OK
                    s = self.list_size(pl[1].h)
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
                    s = self.list_size(pl[1].h)
                    if pl[0].h < s:
                        focus.call("Window:y+", "notmuch", s - pl[0].h)
                    elif pl[0].h > s:
                        focus.call("Window:y-", "notmuch", pl[0].h - s)
            return 0
    def list_size(self,space):
        ch,ln = self.scale()
        min = 4
        if space * 100 / ln > min * 4:
            h = space / 4
        else:
            h = ln * 10 * min / 1000
            if h > space / 2:
                h = space / 2
        return h



def render_query_attach(key, home, focus, comm2, **a):
    p = focus.render_attach("format")
    p = notmuch_query_view(p)
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
    editor.call("global-set-command", "attach-render-notmuch:query",
                render_query_attach)
    editor.call("global-set-command", "interactive-cmd-nm", notmuch_mode)
