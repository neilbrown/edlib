# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2015 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# edlib display module using pygtk to create a window, handle drawing, and
# receive mouse/keyboard events.
# provides eventloop function using gtk.main.

import sys
import os
import pygtk
import gtk
import pango
import thread
import gobject
import glib

class EdDisplay(gtk.Window):
    def __init__(self, home):
        gtk.Window.__init__(self)
        self.pane = edlib.Pane(home, self.handle, None)
        self.pane.w = 80
        self.pane.h = 24
        self.panes = {}
        self.set_default_size(700, 300)
        self.set_title("EDLIB")
        self.connect('destroy', self.close_win)
        self.create_ui()
        self.show()

    def handle(self, key, **a):
        if key == "pane-text":
            (x,y) = a["xy"]
            c = a["extra"]
            f = a["focus"]
            if "str2" in a:
                attr = a["str2"]
            else:
                attr = ""
            pm = self.get_pixmap(f);
            self.draw_char(pm, x, y, c, attr)
            return True
        if key == "pane-clear":
            f = a["focus"]
            pm = self.get_pixmap(f)
            self.do_clear(pm)
            return True
        if key == "Notify:Close":
            f = a["focus"]
            if f and f in self.panes:
                del self.panes[f]
            return True

        return None

    def get_pixmap(self, p):
        if p in self.panes:
            pm = self.panes[p]
            (w,h) = pm.get_size()
            if w == p.w * self.charwidth and h == p.h * self.lineheight:
                return pm
            del self.panes[p]
        else:
            self.pane.add_notify(p, "Notify:Close")

        self.panes[p] = gtk.gdk.Pixmap(self.window, p.w * self.charwidth, p.h * self.lineheight)
        return self.panes[p]

    def close_win(self, *a):
        self.destroy()

    def create_ui(self):
        text = gtk.DrawingArea()
        self.text = text
        self.add(text)
        text.show()
        self.fd = pango.FontDescription("mono 12")
        text.modify_font(self.fd)
        ctx = text.get_pango_context()
        metric = ctx.get_metrics(self.fd)
        self.lineheight = (metric.get_ascent() + metric.get_descent()) / pango.SCALE
        self.charwidth = metric.get_approximate_char_width() / pango.SCALE
        self.set_default_size(self.charwidth * 80, self.lineheight * 24)
        self.gc = None
        self.bg = None

        self.text.connect("expose-event", self.refresh)
        self.text.connect("button-press-event", self.press)
        self.text.connect("key-press-event", self.keystroke)
        self.text.connect("configure-event", self.reconfigure)
        self.text.set_events(gtk.gdk.EXPOSURE_MASK|
                             gtk.gdk.STRUCTURE_MASK|
                             gtk.gdk.BUTTON_PRESS_MASK|
                             gtk.gdk.BUTTON_RELEASE_MASK|
                             gtk.gdk.KEY_PRESS_MASK|
                             gtk.gdk.KEY_RELEASE_MASK);
        self.text.set_property("can-focus", True)


    def refresh(self, *a):
        self.pane.refresh()
        l = self.panes.keys()
        l.sort(key=lambda pane: pane.abs_z)
        for p in l:
            pm = self.panes[p]
            (rx,ry) = p.abs(0,0)
            self.text.window.draw_drawable(self.bg, pm, 0, 0,
                                           rx * self.charwidth, ry * self.lineheight,
                                           -1, -1)

    def reconfigure(self, w, ev):
        alloc = w.get_allocation()
        rows = int(alloc.height / self.lineheight)
        cols = int(alloc.width / self.charwidth)
        self.pane.w = cols
        self.pane.h = rows
        self.text.queue_draw()

    def press(self, c, event):
        c.grab_focus()
        x = int(event.x / self.charwidth)
        y = int(event.y / self.lineheight)
        s = "Click-" + ("%d"%event.button)
        if event.state & gtk.gdk.SHIFT_MASK:
            s = "S-" + s;
        if event.state & gtk.gdk.CONTROL_MASK:
            s = "C-" + s;
        if event.state & gtk.gdk.MOD1_MASK:
            s = "M-" + s;
        self.pane.call_xy("Mouse-event", "Click-1", self.pane, (x,y))
        self.refresh()

    eventmap = { "Return" : "Return",
                 "Tab" : "Tab",
                 "Escape" : "ESC",
                 "Linefeed" : "LF",
                 "Down" : "Down",
                 "Up" : "Up",
                 "Left" : "Left",
                 "Right" : "Right",
                 "Home" : "Home",
                 "End" : "End",
                 "BackSpace" : "Backspace",
                 "Delete" : "Del",
                 "Insert" : "Ins",
                 "Page_Up" : "Prior",
                 "Page_Down" : "Next",
                 }
    def keystroke(self, c, event):
        kv = gtk.gdk.keyval_name(event.keyval)
        if kv in self.eventmap:
            s = self.eventmap[kv]
            if event.state & gtk.gdk.SHIFT_MASK:
                s = "S-" + s;
            if event.state & gtk.gdk.CONTROL_MASK:
                s = "C-" + s;
        else:
            s = event.string
            if len(s) == 0:
                return
            if ord(s[0]) < 32:
                s = "C-Chr-" + chr(ord(s[0])+64)
            else:
                s = "Chr-" + s
        if event.state & gtk.gdk.MOD1_MASK:
            s = "M-" + s;
        self.pane.call_focus("Keystroke", self.pane, s)
        self.pane.refresh()

    def draw_char(self, pm, x, y, c, attr):
        t = self.text
        if not self.gc:
            self.gc = t.window.new_gc()
            cmap = t.get_colormap()
            self.gc.set_foreground(cmap.alloc_color(gtk.gdk.color_parse("blue")))
        if not self.bg:
            self.bg = t.window.new_gc()
            cmap = t.get_colormap()
            self.bg.set_foreground(cmap.alloc_color(gtk.gdk.color_parse("lightyellow")))
        layout = t.create_pango_layout(unichr(c))
        bg = self.bg; fg = self.gc
        if "inverse" in attr:
            fg,bg = bg,fg
        pm.draw_rectangle(bg, True, x * self.charwidth, y * self.lineheight, self.charwidth, self.lineheight)
        pm.draw_layout(fg, x * self.charwidth, y * self.lineheight, layout)
        t.queue_draw()
        return False

    def do_clear(self, pm):

        t = self.text
        if not self.bg:
            self.bg = t.window.new_gc()
            cmap = t.get_colormap()
            self.bg.set_foreground(cmap.alloc_color(gtk.gdk.color_parse("lightyellow")))
        (w,h) = pm.get_size()
        pm.draw_rectangle(self.bg, True, 0, 0, w, h)

def new_display(key, home, comm2, **a):
    disp = EdDisplay(home)
    comm2('callback', disp.pane)
    return 1

class events:
    def __init__(self):
        self.active = True

    def read(self, key, focus, comm2, numeric, **a):
        gobject.io_add_watch(numeric, gobject.IO_IN, self.docall, comm2, focus, numeric)

    def docall(self, comm, focus, fd):
        comm2.call("callback", focus, fd)
        return 1

    def signal(self, key, focus, comm2, numeric, **a):
        pass

    def run(self, key, **a):
        if self.active:
            gtk.main()
            return 1
        else:
            return 0

    def deactivate(self, key, **a):
        self.active = False
        gtk.main_quit()
        return 1

def events_activate(key, home, focus, **a):
    ev = events()
    home.call("global-set-command", focus, "event:read", ev.read)
    home.call("global-set-command", focus, "event:signal", ev.signal)
    home.call("global-set-command", focus, "event:run", ev.run)
    home.call("global-set-command", focus, "event:deactivate", ev.deactivate)

    return 1

editor.call("global-set-command", pane, "display-pygtk", new_display);
editor.call("global-set-command", pane, "pygtkevent:activate", events_activate);
