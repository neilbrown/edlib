#!/usr/bin/env python
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
				self.sock.close()
				self.sock = None
				self.close()
				return edlib.Efalse
			else:
				if msg[:5] == "open:":
					path = msg[5:]
					# 8==reload
					d = editor.call("doc:open", -1, 8, path, ret = "focus")
					if not d:
						self.sock.send("FAIL")
						return 1
					if self.term:
						p = self.term.call("doc:attach", ret='focus')
						p = p.call("doc:assign-view", d, ret='focus')
						p.take_focus()
						self.sock.send("OK")
                                                return 1
				        self.call("Call:Notify:global-displays", self.display_callback)
					if self.destpane:
						p = self.destpane
						self.destpane = None
						while p.focus:
							p = p.focus
						p = p.call("PopupTile", "MD3t", ret='focus')
						#p = p.call("ThisPane", ret='focus')
						p = p.call("doc:attach", ret='focus')
						p = p.call("doc:assign-view", d, ret='focus')
						p.take_focus()
						self.sock.send("OK")
					else:
						self.sock.send("No Display!")
					return 1
				if msg[:24] == "Request:Notify:doc:done:":
					path = msg[24:]
					d = editor.call("doc:open", -1, path, ret="focus")
					if not d:
						self.sock.send("FAIL")
						return 1
					self.add_notify(d, "Notify:doc:done")
					if self.term:
					    self.term.call("Display:set-noclose",
					                   "Cannot close display until document done - use 'C-x #'")
					self.sock.send("OK")
					return 1
                                if msg == "Request:Notify:Close":
                                        if self.term:
                                                # trigger finding a new document
                                                self.term.call("Window:bury")
                                        self.want_close = True
                                        self.sock.send("OK")
                                        return 1
                                if msg[:5] == "term:":
                                        path = msg[5:]
                                        p = editor.call("attach-input", ret='focus')
                                        p = p.call("attach-display-ncurses", path,
                                                        "xterm", ret="focus")
                                        self.disp = p
                                        p = p.call("attach-messageline", ret='focus')
                                        p = p.call("attach-global-keymap", ret='focus')
                                        p.call("attach-mode-emacs")
                                        p = p.call("attach-tile", ret='focus')
                                        p.take_focus()
                                        self.term = p
                                        self.add_notify(self.disp, "Notify:Close")
                                        self.sock.send("OK")
                                        return 1
                                if msg == "Close":
                                        if self.disp:
                                                self.disp.close()
                                                self.disp = None
				        self.sock.close()
				        self.sock = None
				        self.close()
                                        return 1
				self.sock.send("Unknown")
				return 1

                def handle_term_close(self, key, focus, **a):
                        "handle:Notify:Close"
                        if focus == self.disp:
                                self.disp = None
                                self.term = None
                        if self.want_close:
                                self.sock.send("Close")
                                self.want_close = False
                        return 0

		def handle_done(self, key, str, **a):
			"handle:Notify:doc:done"
			if str != "test":
			    if self.term:
			        self.term.call("Window:set-noclose")
			    self.sock.send("Done")
			return 1

		def display_callback(self, key, focus, num, **a):
			self.destpane = focus
			return 0

		def handle_close(self, key, **a):
			"handle:Close"
			if self.sock:
                                if self.want_close:
                                        self.sock.send("Close")
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
                        print "Usage: edlibclient [-t] filename"
                        sys.exit(1)
        if not term and not file:
                print "edlibclient: must provide -t or filename (or both)"
                sys.exit(1)

	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	try:
		s.connect(sockpath)
	except OSError:
		print "Cannot connect to ".sockpath
		os.exit(1)

        if term:
                t = os.ttyname(0)
                if t:
                        s.send("term:" + t)
                        ret = s.recv(100)
                        if ret != "OK":
                                print "Cannot open terminal on", t
                                sys.exit(1)

        if file:
	        file = os.path.realpath(file)
	        s.send("open:" + file)
	        ret = s.recv(100)
	        if ret != "OK":
		        print "Cannot open: ", ret
		        sys.exit(1)
	        s.send("Request:Notify:doc:done:"+file)
        else:
                s.send("Request:Notify:Close")
        ret = s.recv(100)
        if ret != "OK":
	        print "Cannot request notification: ", ret
	        sys.exit(1)
        ret = s.recv(100)
        if ret != "Done" and ret != "Close":
	        print "Received unexpected notification: ", ret
	        sys.exit(1)
        if ret != "Close":
                s.send("Close")
                s.recv(100);
        s.close()
        sys.exit(0)
else:
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	def server_accept(key, **a):
		(new, addr) = s.accept()
		ServerPane(new)

	def server_done(key, focus, **a):
		ret = focus.call("Call:Notify:doc:done", "test")
		if ret > 0:
			# maybe save, then notify properly
			fn = focus["filename"]
			mod = focus["doc-modified"]
			if fn and mod == "yes":
				focus.call("Message", "Please save first!")
			else:
				focus.call("Call:Notify:doc:done")
				# FIXME need something better than 'bury'
				# If it was already visible, it should stay that way
				focus.call("Window:bury")
		else:
			# Find and visit a doc waiting to be done
			choice = []
			def chose(choice, a):
				focus = a['focus']
				if focus.notify("Notify:doc:done", "test") > 0:
					choice.append(focus)
					return 1
				return 0
			focus.call("docs:byeach", lambda key,**a:chose(choice, a))
			if len(choice):
				par = focus.call("ThisPane", ret='focus')
				if par:
					par = par.call("doc:attach", ret='focus')
					par = par.call("doc:assign-view", choice[0], ret='focus')
					par.take_focus()

		return 1

	try:
	  os.unlink(sockpath)
	except OSError:
	  pass
	mask = os.umask(077)
	s.bind(sockpath)
	os.umask(mask)
	s.listen(5)
	editor.call("global-set-command", "lib-server:done", server_done)
	editor.call("event:read", s.fileno(), server_accept)
