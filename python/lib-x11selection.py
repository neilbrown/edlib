# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib module to provide access to X11 selections for cut/paste
# This can be used with any display including ncurses, providing
# an Xserver is accessible

import os
import gi
gi.require_version('Gtk', '3.0')
from gi.repository import Gtk, GLib, Gdk

class X11Sel(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)
        self.primary_cb = Gtk.Clipboard.get(Gdk.SELECTION_PRIMARY)
        if self.primary_cb == None:
            raise edlib.Commandfailed
        self.clipboard_cb = Gtk.Clipboard.get(Gdk.SELECTION_CLIPBOARD)
        self.primary_cb.connect("owner-change", self.cb_new_owner, "PRIMARY")
        self.clipboard_cb.connect("owner-change", self.cb_new_owner, "CLIPBOARD")
        self.targets = [ (Gdk.SELECTION_TYPE_STRING, 0, 0) ]
        self.have_primary = False; self.set_primary = False
        self.have_clipboard = False; self.set_clipboard = False
    def claim_primary(self, str):
        if self.have_primary:
            return
        self.primary_cb.set_text(str, len(str))
        self.have_primary = True
        self.set_primary = True

    def claim_both(self, str):
        self.claim_primary(str)
        if self.have_clipboard:
            return
        self.clipboard_cb.set_text(str, len(str))
        self.have_clipboard = True
        self.set_clipboard = True

    def request_clip(self, sel, seldata, info, data):
        s = self.parent.call("copy:get", 0, ret='str')
        if not s:
            s = ""
        seldata.set_text(s)

    def cb_new_owner(self, cb, ev, data):
        if data == "PRIMARY":
            if self.set_primary:
                # I must be the new owner
                self.set_primary = False
            else:
                self.have_primary = False
        if data == "CLIPBOARD":
            if self.set_clipboard:
                self.set_clipboard = False
            else:
                self.have_clipboard = False

    def copy_save(self, key, num2, str, **a):
        "handle:copy:save"
        if num2:
            # mouse-only
            self.claim_primary(str)
        else:
            self.claim_both(str)
        return 0

    def copy_get(self, key, focus, num, comm2, **a):
        "handle:copy:get"
        if not self.have_primary:
            if num == 0:
                s = self.primary_cb.wait_for_text()
                if s is not None:
                    if comm2:
                        comm2("cb", focus, s)
                    return 1
            else:
                if self.primary_cb.wait_is_text_available():
                    num -= 1
        if not self.have_clipboard:
            if num == 0:
                s = self.clipboard_cb.wait_for_text()
                if s is not None:
                    if comm2:
                        comm2("cb", focus, s)
                    return 1
            else:
                if self.clipboard_cb.wait_is_text_available():
                    num -= 1
        return self.parent.call(key, focus, num, comm2)

def new_sel(key, focus, comm2, **a):
    if not 'DISPLAY' in os.environ:
        return None
    if not os.environ['DISPLAY']:
        return None
    focus.call("attach-glibevents")
    p = X11Sel(focus)
    if p:
        comm2('callback', p)
        return 1
    return None

editor.call("global-set-command", "attach-x11selection", new_sel);
