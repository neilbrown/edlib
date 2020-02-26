#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import socket, os, sys

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
                    # 8==reload
                    d = editor.call("doc:open", -1, 8, path, ret = "focus")
                    if not d:
                        self.sock.send(b"FAIL")
                        return 1
                    if self.term:
                        d.call("doc:attach-view", self.term, 1,
                               ret='focus')
                        self.term.take_focus()
                        self.sock.send(b"OK")
                        return 1
                    self.display_time = 0
                    self.destpane = None
                    self.call("editor:notify:all-displays", self.display_callback)
                    if self.destpane:
                        p = self.destpane
                        self.destpane = None
                        while p.focus:
                            p = p.focus
                        p = p.call("PopupTile", "MD3tsa", ret='focus')
                        #p = p.call("ThisPane", ret='focus')
                        d.call("doc:attach-view", p, 1, ret='focus')
                        p.take_focus()
                        self.sock.send(b"OK")
                    else:
                        self.sock.send(b"No Display!")
                    return 1
                if msg[:21] == b"doc:request:doc:done:":
                    path = msg[21:].decode("utf-8")
                    d = editor.call("doc:open", -1, path, ret="focus")
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
                if msg[:5] == b"term:":
                    path = msg[5:].decode("utf-8")
                    p = editor.call("attach-input", ret='focus')
                    p = p.call("attach-display-ncurses", path,
                               "xterm-256color", ret="focus")
                    self.disp = p
                    p = p.call("attach-messageline", ret='focus')
                    p = p.call("attach-global-keymap", ret='focus')
                    p.call("attach-mode-emacs")
                    p = p.call("attach-tile", ret='focus')
                    p.take_focus()
                    self.term = p
                    self.add_notify(self.disp, "Notify:Close")
                    self.sock.send(b"OK")
                    return 1
                if msg == b"Close":
                    if self.disp:
                        self.disp.close()
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
            return 0

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
            return 0

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
        print("Cannot connect to ".sockpath)
        os.exit(1)

    if term:
        t = os.ttyname(0)
        if t:
            s.send(b"term:" + t.encode("utf-8"))
            ret = s.recv(100)
            if ret != b"OK":
                print("Cannot open terminal on", t)
                sys.exit(1)

    if file:
        file = os.path.realpath(file)
        s.send(b"open:" + file.encode("utf-8"))
        ret = s.recv(100)
        if ret != b"OK":
            print("Cannot open: ", ret.decode("utf-8"))
            sys.exit(1)
        s.send(b"doc:request:doc:done:"+file.encode("utf-8"))
    else:
        s.send(b"Request:Notify:Close")
    ret = s.recv(100)
    if ret != b"OK":
        print("Cannot request notification: ", ret.decode('utf-8'))
        sys.exit(1)
    ret = s.recv(100)
    if ret != b"Done" and ret != b"Close":
        print("Received unexpected notification: ", ret.decode('utf-8'))
        sys.exit(1)
    if ret != b"Close":
        s.send(b"Close")
        s.recv(100);
    s.close()
    sys.exit(0)
else:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    def server_accept(key, **a):
        (new, addr) = s.accept()
        ServerPane(new)

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
                par = focus.call("ThisPane", ret='focus')
                if par:
                    par = choice[0].call("doc:attach-view", par, 1, ret='focus')
                    par.take_focus()

        return 1

    try:
        os.unlink(sockpath)
    except OSError:
        pass
    mask = os.umask(0o077)
    s.bind(sockpath)
    os.umask(mask)
    s.listen(5)
    editor.call("global-set-command", "lib-server:done", server_done)
    editor.call("event:read", s.fileno(), server_accept)
