#!/usr/bin/env python3
# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# Server protocol (subject to change):
# client sends:  cmd  arg  env    - all '\0' separated
# server replies OK or FAIL or "Close" or "Done"
#
# cmd:
#   goto-line NN     - sets line number
#   open FILENAME    - open and view the file, going to given line if set
#   request-done FILENAME - request notification when file "done" (cx #)
#   request-close    - send notify when display is closed (K:C-X 5 0)
#   x11window        - create x11window on display ARG with ENV
#   term             - open ncurse display on tty ARG with ENV
#   sig-winch        - terminal changed size - refresh
#   close            - close display and connection
#
import socket, os, sys, fcntl, signal

if 'EDLIB_SOCK' in os.environ:
    sockpath = os.environ['EDLIB_SOCK']
else:
    sockpath = "/tmp/edlib-neilb"

if sys.argv[0] == "":
    # server
    import edlib

    class ServerPane(edlib.Pane):
        # This pane receives requests on a socket and
        # forwards them to the editor.  When a notification
        # arrives, it is sent back to the client
        def __init__(self, sock):
            edlib.Pane.__init__(self, edlib.editor)
            self.sock = sock
            self.term = None
            self.disp = None
            self.doc = None
            self.want_close = False
            self.lineno = None
            edlib.editor.call("event:read", sock.fileno(),
                        self.read)

        def read(self, key, **a):
            if self.sock:
                msg = self.sock.recv(1000)
            else:
                msg = None

            if not msg :
                if self.disp:
                    self.disp.call("Display:close")
                if self.sock:
                    self.sock.close()
                self.sock = None
                self.close()
                return edlib.Efalse
            else:
                words = msg.split(b'\0')
                cmd = words[0].decode('utf-8','ignore')
                arg = None
                env={}
                if len(words) > 1:
                    arg = words[1].decode('utf-8','ignore')
                for v in words[2:]:
                    vw = v.split(b'=')
                    if len(vw) == 2 and vw[0] in [b'TERM',
                                                  b'DISPLAY',
                                                  b'XAUTHORITY',
                                                  b'REMOTE_SESSION']:
                        env[vw[0].decode("utf-8")] = vw[1].decode("utf-8",'ignore')

                if cmd == "goto-line":
                    try:
                        self.lineno = int(arg)
                    except ValueError:
                        self.lineno = None
                    self.sock.send(b'OK')
                    return 1
                if cmd == "open":
                    path = arg
                    try:
                        # 8==reload
                        d = edlib.editor.call("doc:open", -1, 8, path, ret='pane')
                    except edlib.commandfailed:
                        d = None
                    if not d:
                        self.sock.send(b"FAIL")
                        return 1
                    if self.term:
                        p = self.term.leaf.call("ThisPane", ret='pane')
                        p = d.call("doc:attach-view", p, 1, ret='pane')
                        self.term.take_focus()
                        self.sock.send(b"OK")
                        if self.lineno != None:
                            pt = p.call("doc:point", ret='mark')
                            p.call("CountLines", self.lineno, pt, "goto:line")
                        return 1
                    p = self.choose_pane()
                    if not p:
                        self.sock.send(b"No Display!")
                        return 1
                    p = p.call("PopupTile", "MD3tsa", ret='pane')
                    if not p:
                        self.sock.send(b"No tile!")
                        return 1
                    p2 = d.call("doc:attach-view", p, 1, ret='pane')
                    p.take_focus()
                    self.sock.send(b"OK")
                    if self.lineno != None:
                        pt = p2.call("doc:point", ret='mark')
                        p2.call("CountLines", self.lineno, pt, "goto:line")
                    return 1
                if cmd == "request-done":
                    path = arg
                    d = edlib.editor.call("doc:open", -1, path, ret='pane')
                    if not d:
                        self.sock.send(b"FAIL")
                        return 1
                    self.add_notify(d, "doc:done")
                    self.add_notify(d, "Notify:Close")
                    self.doc = d
                    if self.term:
                        self.term.call("Display:set:no-close",
                                       "Cannot close display until document done - use 'C-x #'")
                    self.sock.send(b"OK")
                    return 1
                if  cmd == "request-close":
                    if self.term:
                        # trigger finding a new document
                        self.term.call("Window:bury")
                    self.want_close = True
                    self.sock.send(b"OK")
                    return 1
                if cmd == 'x11window' and not self.term:
                    p = edlib.editor.call("interactive-cmd-x11window",
                                    arg, env['XAUTHORITY'], ret='pane')
                    if p:
                        for v in env:
                            p.call("window:set:", env[v], v)
                        p.call("Window:bury")

                    self.term = p
                    self.sock.send(b'OK')
                    return 1
                if cmd == 'term' and not self.term:
                    path = arg
                    p = edlib.editor
                    p = p.call("attach-display-ncurses", path, env['TERM'],
                               ret='pane')
                    self.term = p
                    for v in env:
                        self.term.call("window:set:", env[v], v)
                    self.disp = self.term
                    self.add_notify(self.disp, "Notify:Close")
                    self.sock.send(b"OK")
                    return 1
                if cmd == "sig-winch":
                    if self.term:
                        self.term.call("Sig:Winch")
                        self.sock.send(b"OK")
                    else:
                        self.sock.send(b"Unknown")
                    return 1
                if cmd == "close":
                    if self.disp:
                        self.disp.call("Display:set:no-close")
                        self.disp.call("Display:close")
                        self.disp = None
                    self.call("event:free", self.read)
                    self.sock.close()
                    self.sock = None
                    self.close()
                    return edlib.Efalse
                if cmd == "askpass":
                    def cb(key, str1, **a):
                        if str1:
                            self.sock.send(str1.encode())
                        self.sock.close()
                        self.sock = None

                    p = self.choose_pane()
                    if p:
                        p.call("AskPass", arg, cb)
                    return 1
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
                    self.term.call("Display:set:no-close")
                    self.term.call("Display:close")
                self.sock.send(b"Done")
            return 1

        def handle_done(self, key, str, **a):
            "handle:doc:done"
            if str != "test":
                if self.term:
                    self.term.call("Display:set:no-close")
                    self.term.call("Display:close")
                self.sock.send(b"Done")
            return 1

        def choose_pane(self):
            self.display_time = 0
            self.destpane = None
            self.call("editor:notify:all-displays", self.display_callback)
            if self.destpane:
                p = self.destpane.leaf
                self.destpane = None
                # Need to avoid transient popups
                if p:
                    p = p.call("ThisPane", ret='pane')
            else:
                p = None
            return p

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
                self.disp.call("Display:close")
                self.disp = None

    global server_sock
    server_sock = None
    def server_accept(key, **a):
        global server_sock
        try:
            (new, addr) = server_sock.accept()
            ServerPane(new)
        except:
            pass
        return 1

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
            def choose(choice, a):
                focus = a['focus']
                if focus.notify("doc:done", "test") > 0:
                    choice.append(focus)
                    return 1
                return 0
            focus.call("docs:byeach", lambda key,**a:choose(choice, a))
            if len(choice):
                par = focus.call("ThisPane", ret='pane')
                if par:
                    par = choice[0].call("doc:attach-view", par, 1, ret='pane')
                    par.take_focus()

        return 1

    def server_rebind(key, focus, **a):
        global server_sock

        msg = ""
        if key.startswith("interactive-cmd"):
            msg = "Server started"
        if server_sock:
            # stop reading this file
            focus.call("event:free", server_accept)
            server_sock.close()
            server_sock = None
            msg="Server restarted"

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

        focus.root.call("event:read", s.fileno(), server_accept)
        server_sock = s
        if key != "key":
            focus.call("Message", msg)
        return 1

    def server_autostart(key, focus, **a):
        if focus["server:autostart"] == 'yes':
            server_rebind(key, focus)

    edlib.editor.call("global-set-command", "startup-server", server_autostart)
    edlib.editor.call("global-set-command", "lib-server:done", server_done)
    edlib.editor.call("global-set-command", "interactive-cmd-server-start",
                server_rebind)

elif 'askpass' in sys.argv[0]:
    msg = sys.argv[1]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sockpath)
    except OSError:
        print("Cannot connect to ",sockpath)
        sys.exit(1)

    s.send(b"askpass\0" + msg.encode())
    ret = s.recv(1000)
    print(ret.decode('utf-8', 'ignore'))
    sys.exit(0)
else:
    term = False
    file = None
    lineno = None
    x11 = False
    for a in sys.argv[1:]:
        if a == "-t":
            term = True
        elif a == "-x":
            x11 = True
        elif a[0] == '+':
            lineno = a[1:]
        elif file is None:
            file = a
        else:
            print("Usage: edlibclient [-t] filename")
            sys.exit(1)
    if not term and not x11 and not file:
        print("edlibclient: must provide -t, -x, or filename")
        sys.exit(1)

    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(sockpath)
    except OSError:
        print("Cannot connect to ",sockpath)
        sys.exit(1)

    winch_ok = False

    if x11:
        if 'DISPLAY' in os.environ:
            d = os.environ['DISPLAY']
        else:
            print("edlibclient: -x not supported unless $DISPLAY is set")
            sys.exit(1)
        m = [ "x11window", d]
        for i in ['XAUTHORITY']:
            if i in os.environ:
                m.append(i + "=" + os.environ[i])
        s.send('\0'.join(m).encode('utf-8'))
        ret = s.recv(100)
        if ret != b'OK':
            print("Cannot start x11 display:" + ret.decode('utf-8','ignore'))

    if term:
        t = os.ttyname(0)
        if t:
            m = ["term", t]
            for i in ['TERM','DISPLAY','XAUTHORITY']:
                if i in os.environ:
                    m.append(i + "=" + os.environ[i])
            if 'SSH_CONNECTION' in os.environ:
                m.append("REMOTE_SESSION=yes")

            s.send('\0'.join(m).encode('utf-8'))
            ret = s.recv(100)
            if ret != b"OK":
                print("Cannot open terminal on", t)
                s.send(b"close")
                s.recv(100)
                sys.exit(1)
            def handle_winch(sig, frame):
                if winch_ok:
                    s.send(b"sig-winch")
                return 1
            signal.signal(signal.SIGWINCH, handle_winch)

    if file:
        if lineno is not None:
            s.send(b"goto-line\0" + lineno.encode('utf-8'))
            ret = s.recv(100)
        file = os.path.realpath(file)
        s.send(b"open\0" + file.encode("utf-8"))
        ret = s.recv(100)
        if ret != b"OK":
            s.send(b"close")
            s.recv(100)
            print("Cannot open: ", ret.decode("utf-8", 'ignore'))
            sys.exit(1)
        s.send(b"request-done\0"+file.encode("utf-8"))
    elif term:
        s.send(b"request-close")
    else:
        sys.exit(0)
    ret = s.recv(100)
    if ret != b"OK":
        print("Cannot request notification: ", ret.decode('utf-8', 'ignore'))
        s.send(b"close")
        s.recv(100)
        sys.exit(1)
    winch_ok = True
    while True:
        ret = s.recv(100)
        if ret != b"OK":
            break
        # probably a reply to sig-winch
    winch_ok = False
    if ret and ret != b"Done" and ret != b"Close":
        print("Received unexpected notification: ", ret.decode('utf-8', 'ignore'))
        s.send(b"close")
        s.recv(100)
        sys.exit(1)
    if ret and ret != b"Close":
        s.send(b"close")
        s.recv(100)
    s.close()
    sys.exit(0)

