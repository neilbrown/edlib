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
        self.panes = {}
        self.set_title("EDLIB")
        self.connect('destroy', self.close_win)
        self.create_ui()
        self.pane.w = self.charwidth * 80
        self.pane.h = self.lineheight * 24
        self.show()

    def handle(self, key, **a):
        if key == "pane-clear":
            f = a["focus"]
            if "str2" in a:
                fg, bg = self.get_colours(a["str2"])
            else:
                fg, bg = self.get_colours("")
            pm = self.get_pixmap(f)
            self.do_clear(pm, bg)
            return True

        if key == "text-size":
            fd = self.extract_font(a["str2"])
            ctx = self.text.get_pango_context()
            metric = ctx.get_metrics(fd)
            self.text.modify_font(fd)
            layout = self.text.create_pango_layout(a["str"])
            ink,(x,y,width,height) = layout.get_pixel_extents()
            ascent = metric.get_ascent() / pango.SCALE
            cb = a["comm2"]
            if a['numeric'] >= 0:
                if width <= a['numeric']:
                    max_bytes = len(a["str"])
                else:
                    max_bytes,extra = layout.xy_to_index(pango.SCALE*a["numeric"],
                                                         metric.get_ascent())
            else:
                max_bytes = 0
            f = a["focus"]
            return cb("callback:size", f, max_bytes, ascent, (width, height))

        if key == "text-display":
            if not self.gc:
                t = self.text
                self.gc = t.window.new_gc()
                cmap = t.get_colormap()
                self.gc.set_foreground(cmap.alloc_color(gtk.gdk.color_parse("blue")))

            (x,y) = a["xy"]
            f = a["focus"]
            if 'str2' in a:
                attr = a['str2']
            else:
                attr = ''
            fd = self.extract_font(attr)
            ctx = self.text.get_pango_context()
            self.text.modify_font(fd)
            layout = self.text.create_pango_layout(a["str"])
            fg, bg = self.get_colours(attr)
            pm = self.get_pixmap(f)
            metric = ctx.get_metrics(fd)
            ascent = metric.get_ascent() / pango.SCALE
            pm.draw_layout(self.gc, x, y-ascent, layout, fg, bg)
            if a['numeric'] >= 0:
                cx,cy,cw,ch = layout.index_to_pos(a["numeric"])
                if cw <= 0:
                    cw = metric.get_approximate_char_width()
                cx /= pango.SCALE
                cy /= pango.SCALE
                cw /= pango.SCALE
                ch /= pango.SCALE
                pm.draw_rectangle(self.gc, False, x+cx, y-ascent+cy,
                                  cw-1, ch-1);
                extra = True
                while f.parent and f.parent.parent:
                    if f.parent.focus != f:
                        extra = False
                    f = f.parent
                if extra:
                    pm.draw_rectangle(self.gc, False, x+cx+1, y-ascent+cy+1,
                                      cw-3, ch-3);

        if key == "Notify:Close":
            f = a["focus"]
            if f and f in self.panes:
                del self.panes[f]
            return True

        return None

    styles=["oblique","italic","bold","small-caps"]

    def extract_font(self, attrs):
        "Return a pango.FontDescription"
        family="mono"
        style=""
        size=10
        for word in attrs.split(','):
            if word in self.styles:
                style += " " + word
            elif word == "large":
                size = 14
            elif word[0:7] == "family:":
                family = word[7:]
        return pango.FontDescription(family+' '+style+' '+str(size))

    def get_colours(self, attrs):
        "Return a foreground and a background colour"
        fg = "black"
        bg = "white"
        inv = False
        for word in attrs.split(','):
            if word[0:3] == "fg:":
                fg = word[3:]
            if word[0:3] == "bg:":
                bg = word[3:]
            if word == "inverse":
                inv = True
        cmap = self.text.get_colormap()
        fgc = cmap.alloc_color(gtk.gdk.color_parse(fg))
        bgc = cmap.alloc_color(gtk.gdk.color_parse(bg))
        if inv:
            return (bgc, fgc)
        else:
            return (fgc, bgc)

    def get_pixmap(self, p):
        if p in self.panes:
            pm = self.panes[p]
            (w,h) = pm.get_size()
            if w == p.w and h == p.h:
                return pm
            del self.panes[p]
        else:
            self.pane.add_notify(p, "Notify:Close")

        self.panes[p] = gtk.gdk.Pixmap(self.window, p.w, p.h)
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
                                           rx, ry,
                                           -1, -1)

    def reconfigure(self, w, ev):
        alloc = w.get_allocation()
        self.pane.w = alloc.width
        self.pane.h = alloc.height
        self.text.queue_draw()

    def press(self, c, event):
        c.grab_focus()
        x = int(event.x)
        y = int(event.y)
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

    def do_clear(self, pm, colour):

        t = self.text
        if not self.bg or True:
            self.bg = t.window.new_gc()
            cmap = t.get_colormap()
            self.bg.set_foreground(colour)
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
