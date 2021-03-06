#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019-2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import socket, os, sys, fcntl, signal

if 'EDLIB_SOCK' in os.environ:
    sockpath = os.environ['EDLIB_SOCK']
else:
    sockpath = "/tmp/edlib-neilb"

try:
    class ServerPane(edlib.Pane):
        # This pane received requests on a socket and
        # forwards them to the editor.  When a notification
        # arrives, it is sent back to the client
        def __init__(self, sock):
            edlib.Pane.__init__(self, editor)
            self.sock = sock
            self.term = None
            self.disp = None
            self.doc = None
            self.want_close = False
            editor.call("event:read", sock.fileno(),
                        self.read)

        def read(self, key, **a):
            if self.sock:
                msg = self.sock.recv(1000)
            else:
                msg = None

            if not msg :
                if self.disp:
                    self.disp.close()
                if self.sock:
                    self.sock.close()
                self.sock = None
                self.close()
                return edlib.Efalse
            else:
                if msg[:5] == b"open:":
                    path = msg[5:].decode("utf-8")
                    try:
                        # 8==reload
                        d = editor.call("doc:open", -1, 8, path, ret='pane')
                    except edlib.commandfailed:
                        d = None
                    if not d:
                        self.sock.send(b"FAIL")
                        return 1
                    if self.term:
                        d.call("doc:attach-view", self.term, 1,
                               ret='pane')
                        self.term.take_focus()
                        self.sock.send(b"OK")
                        return 1
                    self.display_time = 0
                    self.destpane = None
                    self.call("editor:notify:all-displays", self.display_callback)
                    if self.destpane:
                        p = self.destpane.leaf
                        self.destpane = None
                        # Need to avoid transient popups
                        if p:
                            p = p.call("ThisPane", ret='pane')
                        if p:
                            p2 = p.call("PopupTile", "MD3tsa", ret='pane')
                            if p2:
                                p = p2
                        if p:
                            d.call("doc:attach-view", p, 1, ret='pane')
                            p.take_focus()
                            self.sock.send(b"OK")
                        else:
                            self.sock.send(b"No Cannot create pane")
                    else:
                        self.sock.send(b"No Display!")
                    return 1
                if msg[:21] == b"doc:request:doc:done:":
                    path = msg[21:].decode("utf-8")
                    d = editor.call("doc:open", -1, path, ret='pane')
                    if not d:
                        self.sock.send(b"FAIL")
                        return 1
                    self.add_notify(d, "doc:done")
                    self.add_notify(d, "Notify:Close")
                    self.doc = d
                    if self.term:
                        self.term.call("Display:set-noclose",
                                       "Cannot close display until document done - use 'C-x #'")
                    self.sock.send(b"OK")
                    return 1
                if msg == b"Request:Notify:Close":
                    if self.term:
                        # trigger finding a new document
                        self.term.call("Window:bury")
                    self.want_close = True
                    self.sock.send(b"OK")
                    return 1
                if msg.startswith(b"term "):
                    w = msg.split(b' ')
                    path = w[1].decode("utf-8")
                    p = editor

                    env={}
                    for v in w[2:]:
                        vw = v.split(b'=')
                        if len(vw) == 2 and vw[0] in [b'TERM',
                                                      b'DISPLAY',
                                                      b'REMOTE_SESSION']:
                            env[vw[0].decode("utf-8")] = vw[1].decode("utf-8")

                    p = p.call("attach-display-ncurses", path, env['TERM'],
                               ret='pane')
                    for v in env:
                        p[v] = env[v]
                    self.disp = p
                    self.term = self.disp.call("editor:activate-display", ret='pane')
                    self.add_notify(self.disp, "Notify:Close")
                    self.sock.send(b"OK")
                    return 1
                if msg == b"Sig:Winch":
                    if self.term:
                        self.term.call("Sig:Winch")
                        self.sock.send(b"OK")
                    else:
                        self.sock.send(b"Unknown")
                    return 1
                if msg == b"Close":
                    if self.disp:
                        self.disp.call("Display:set-noclose")
                        self.disp.call("Display:close")
                        self.disp = None
                    self.sock.close()
                    self.sock = None
                    self.close()
                    return edlib.Efalse
                self.sock.send(b"Unknown")
                return 1

        def handle_term_close(self, key, focus, **a):
            "handle:Notify:Close"
            if focus == self.disp:
                self.disp = None
                self.term = None
                if self.want_close:
                    self.sock.send(b"Close")
                    self.want_close = False
            if focus == self.doc:
                # same as doc:done
                self.doc = None
                if self.term:
                    self.term.call("Window:set-noclose")
                self.sock.send(b"Done")
            return 1

        def handle_done(self, key, str, **a):
            "handle:doc:done"
            if str != "test":
                if self.term:
                    self.term.call("Window:set-noclose")
                self.sock.send(b"Done")
            return 1

        def display_callback(self, key, focus, num, **a):
            if self.display_time == 0 or num > self.display_time:
                self.destpane = focus
                self.display_time = num
            return 1

        def handle_close(self, key, **a):
            "handle:Close"
            if self.sock:
                if self.want_close:
                    self.sock.send(b"Close")
                self.sock.close()
                self.sock = None
            if self.disp:
                self.disp.close()
                self.disp = None

    is_client = False
except:
    is_client = True

if is_client:
    term = False
    file = None
    for a in sys.argv[1:]:
        if a == "-t":
            term = True
        elif file is None:
            file = a
        else:
            print("Usage: edlibclient [-t] filename")
            sys.exit(1)
    if not term and not file:
        print("edlibclient: must provide -t or filename (or both)")
        sys.exit(1)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sockpath)
    except OSError:
        print("Cannot connect to ",sockpath)
        sys.exit(1)

    winch_ok = False

    if term:
        t = os.ttyname(0)
        if t:
            m = ["term", t]
            for i in ['TERM','DISPLAY']:
                if i in os.environ:
                    m.append(i + "=" + os.environ[i])
            if 'SSH_CONNECTION' in os.environ:
                m.append("REMOTE_SESSION=yes")

            s.send(' '.join(m).encode('utf-8'))
            ret = s.recv(100)
            if ret != b"OK":
                print("Cannot open terminal on", t)
                s.send(b"Close")
                s.recv(100)
                sys.exit(1)
            def handle_winch(sig, frame):
                if winch_ok:
                    s.send(b"Sig:Winch")
                return 1
            signal.signal(signal.SIGWINCH, handle_winch)

    if file:
        file = os.path.realpath(file)
        s.send(b"open:" + file.encode("utf-8"))
        ret = s.recv(100)
        if ret != b"OK":
            s.send(b"Close")
            s.recv(100)
            print("Cannot open: ", ret.decode("utf-8"))
            sys.exit(1)
        s.send(b"doc:request:doc:done:"+file.encode("utf-8"))
    else:
        s.send(b"Request:Notify:Close")
    ret = s.recv(100)
    if ret != b"OK":
        print("Cannot request notification: ", ret.decode('utf-8'))
        s.send(b"Close")
        s.recv(100)
        sys.exit(1)
    winch_ok = True
    while True:
        ret = s.recv(100)
        if ret != b"OK":
            break
        # probably a reply to Sig:Winch
    winch_ok = False
    if ret != b"Done" and ret != b"Close":
        print("Received unexpected notification: ", ret.decode('utf-8'))
        s.send(b"Close")
        s.recv(100)
        sys.exit(1)
    if ret != b"Close":
        s.send(b"Close")
        s.recv(100)
    s.close()
    sys.exit(0)
else:
    global server_sock
    server_sock = None
    def server_accept(key, **a):
        global server_sock
        try:
            (new, addr) = server_sock.accept()
            ServerPane(new)
        except:
            pass

    def server_done(key, focus, **a):
        ret = focus.call("doc:notify:doc:done", "test")
        if ret > 0:
            # maybe save, then notify properly
            fn = focus["filename"]
            mod = focus["doc-modified"]
            if fn and mod == "yes":
                focus.call("Message", "Please save first!")
            else:
                focus.call("doc:notify:doc:done")
                # FIXME need something better than 'bury'
                # If it was already visible, it should stay that way
                focus.call("Window:bury")
        else:
            # Find and visit a doc waiting to be done
            choice = []
            def chose(choice, a):
                focus = a['focus']
                if focus.notify("doc:done", "test") > 0:
                    choice.append(focus)
                    return 1
                return 0
            focus.call("docs:byeach", lambda key,**a:chose(choice, a))
            if len(choice):
                par = focus.call("ThisPane", ret='pane')
                if par:
                    par = choice[0].call("doc:attach-view", par, 1, ret='pane')
                    par.take_focus()

        return 1

    def server_rebind(key, focus, **a):
        global server_sock

        if server_sock:
            # stop reading this file
            focus.call("event:free", server_accept)
            server_sock.close()
            server_sock = None
        try:
            os.unlink(sockpath)
        except OSError:
            pass
        mask = os.umask(0o077)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.bind(sockpath)
        os.umask(mask)
        s.listen(5)

        fl = fcntl.fcntl(s.fileno(), fcntl.F_GETFL)
        fcntl.fcntl(s.fileno(), fcntl.F_SETFL, fl | os.O_NONBLOCK)

        focus.call("event:read", s.fileno(), server_accept)
        server_sock = s
        if key != "key":
            focus.call("Message", "Server restarted")
        return 1
    server_rebind("key", editor)
    editor.call("global-set-command", "lib-server:done", server_done)
    editor.call("global-set-command", "interactive-cmd-server-start",
                server_rebind)
