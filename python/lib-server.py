#!/usr/bin/env python
# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2019 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#

import socket, os, sys

sockpath = "/tmp/edlib-neilb"

try:
	class ServerPane(edlib.Pane):
		# This pane received requests on a socket and
		# forwards them to the editor.  When a notification
		# arrives, it is sent back to the client
		def __init__(self, sock):
			edlib.Pane.__init__(self, editor)
			self.sock = sock
			editor.call("event:read", sock.fileno(),
			            self.read)

		def read(self, key, **a):
			if self.sock:
				msg = self.sock.recv(1000)
			else:
				msg = None

			if not msg :
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
					self.destpane = None
					self.call("Notify:global-displays", self.display_callback)
					if self.destpane:
						p = self.destpane
						self.destpane = None
						while p.focus:
							p = p.focus
						p = p.call("ThisPane", ret='focus')
						p = p.call("doc:attach", ret='focus')
						p = p.call("doc:assign", d, "", 1, ret='focus')
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
					self.sock.send("OK")
					return 1
				self.sock.send("Unknown")
				return 1

		def handle_done(self, key, str, **a):
			"handle:Notify:doc:done"
			if str != "test":
				self.sock.send("Done")
			return 1

		def display_callback(self, key, focus, num, **a):
			self.destpane = focus
			return 0
			
		def handle_close(self, key, **a):
			"handle:Close"
			if self.sock:
				self.sock.close()
			self.sock = None


	is_client = False
except:
	is_client = True

if is_client:
	if len(sys.argv) != 2:
		print "usage: edlibclient filename"
		sys.exit(1)
	file = os.path.realpath(sys.argv[1])
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	try:
		s.connect(sockpath)
	except OSError:
		print "Cannot connect to ".sockpath
		os.exit(1)

	s.send("open:" + file)
	ret = s.recv(100)
	if ret != "OK":
		print "Cannot open: ", ret
		sys.exit(1)
	s.send("Request:Notify:doc:done:"+file)
	ret = s.recv(100)
	if ret != "OK":
		print "Cannot request notification: ", ret
		sys.exit(1)
	ret = s.recv(100)
	if ret != "Done":
		print "Received unexpected notification: ", ret
		sys.exit(1)
	sys.exit(0)
else:
	s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
	def server_accept(key, **a):
		(new, addr) = s.accept()
		ServerPane(new)

	def server_done(key, focus, **a):
		ret = focus.call("Notify:doc:done", "test")
		if ret > 0:
			# maybe save, then notify properly
			fn = focus["filename"]
			mod = focus["doc-modified"]
			if fn and mod == "yes":
				focus.call("Message", "Please save first!")
			else:
				focus.call("Notify:doc:done")
				# FIXME need something better than 'bury'
				focus.call("M-Chr-B")
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
					par = par.call("doc:assign", choice[0], "", 1, ret='focus')
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

