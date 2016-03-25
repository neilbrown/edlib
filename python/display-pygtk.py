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


def take(name, place, args, default=None):
    if name in args:
        place.append(args[name])
    else:
        place.append(default)
    return 1

def attach(p, mode):
    pl=[]
    p.call("attach-%s"%mode, lambda key,**a:take('focus', pl, a))
    return pl[0]

class EdDisplay(gtk.Window):
    def __init__(self, home):
        gtk.Window.__init__(self)
        self.pane = edlib.Pane(home, self.handle)
        self.panes = {}
        self.set_title("EDLIB")
        self.connect('destroy', self.close_win)
        self.create_ui()
        self.need_refresh = True
        self.pane.w = self.charwidth * 80
        self.pane.h = self.lineheight * 24
        self.show()

    def handle(self, key, **a):

        if key == "Display:fullscreen":
            if a['numeric'] > 0:
                self.fullscreen()
            else:
                self.unfullscreen()

        if key == "Display:new":
            disp = a['home']
            newdisp = EdDisplay(disp.parent)
            if disp.focus:
                disp.focus.call("Clone", newdisp.pane)
            return 1

        if key == "Close":
            self.pane.close()
            # FIXME close the window??
            return True

        if key == "pane-clear":
            f = a["focus"]
            if "str2" in a:
                fg, bg = self.get_colours(a["str2"])
            else:
                fg, bg = self.get_colours("bg:white")
            pm = self.get_pixmap(f)
            self.do_clear(pm, bg)
            return True

        if key == "text-size":
            attr=""; scale=1000
            if 'str2' in a:
                attr = a['str2']
            if 'extra' in a:
                scale = a['extra']
            fd = self.extract_font(attr, scale)
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
            if not self.gc or not self.bg:
                fg, bg = self.get_colours("fg:blue,bg:white")
                t = self.text
                if not self.gc:
                    self.gc = t.window.new_gc()
                    self.gc.set_foreground(fg)
                if not self.bg:
                    self.bg = t.window.new_gc()
                    self.bg.set_foreground(bg)

            (x,y) = a["xy"]
            f = a["focus"]
            attr=""; scale=1000
            if 'str2' in a:
                attr = a['str2']
            if 'extra' in a:
                scale = a['extra']
            fd = self.extract_font(attr, scale)
            ctx = self.text.get_pango_context()
            self.text.modify_font(fd)
            layout = self.text.create_pango_layout(a["str"])
            fg, bg = self.get_colours(attr)
            pm = self.get_pixmap(f)
            metric = ctx.get_metrics(fd)
            ascent = metric.get_ascent() / pango.SCALE
            ink,(lx,ly,width,height) = layout.get_pixel_extents()
            if bg:
                self.bg.set_foreground(bg)
                pm.draw_rectangle(self.bg, True, x+lx, y-ascent+ly, width, height)
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
                    pm.draw_rectangle(self.gc, True, x+cx, y-ascent+cy,
                                      cw, ch);
                    c = a["numeric"]
                    if c < len(a["str"]):
                        s = unicode(a["str"][c:], "utf-8")
                        l2 = pango.Layout(ctx)
                        l2.set_text(s[0])
                        fg, bg = self.get_colours(attr+",inverse")
                        pm.draw_layout(self.gc, x+cx, y-ascent+cy, l2, fg, bg)
                    else:
                        pm.draw_rectangle(self.gc, False, x+cx, y-ascent+cy,
                                          cw-1, ch-1)
            return True

        if key == "image-display":
            # 'str' is the file name of an image
            # 'numeric' is '1' if image should be stretch to fill pane
            # if 'numeric is '0', then 'extra' is 'or' of
            #   0,1,2 for left/middle/right in x direction
            #   0,4,8 for top/middle/bottom in y direction
            # only one of these can be used as image will fill pane in other direction.
            fl = a['str']
            f = a['focus']
            stretch = a['numeric']
            pos = a['extra']
            w,h = self.pane.w, self.pane.h
            x,y = 0, 0
            try:
                pb = gtk.gdk.pixbuf_new_from_file(fl)
            except:
                # create a red error image
                pb = gtk.gdk.Pixbuf(gtk.gdk.COLORSPACE_RGB, False, 8, w, h)
                pb.fill(0xff000000)
            if not stretch:
                if pb.get_width() * h > pb.get_height() * w:
                    # image is wider than space, reduce height
                    h2 = pb.get_height() * w / pb.get_width()
                    if pos & 12 == 4:
                        y = (h - h2) / 2
                    if pos & 12 == 8:
                        y = h - h2
                    h = h2
                else:
                    # image is too tall, reduce width
                    w2 = pb.get_width() * h / pb.get_height()
                    if pos & 3 == 1:
                        x = (w - w2) / 2
                    if pos & 3 == 2:
                        x = w - w2
                    w = w2
            scale = pb.scale_simple(w, h, gtk.gdk.INTERP_HYPER)
            pm = self.get_pixmap(f)
            pm.draw_pixbuf(self.gc, scale, 0, 0, x, y)
            return True

        if key == "Notify:Close":
            f = a["focus"]
            if f and f in self.panes:
                del self.panes[f]
            return True

        return None

    styles=["oblique","italic","bold","small-caps"]

    def extract_font(self, attrs, scale):
        "Return a pango.FontDescription"
        family="mono"
        style=""
        size=10
        if scale <= 10:
            scale = 1000
        for word in attrs.split(','):
            if word in self.styles:
                style += " " + word
            elif len(word) and word[0].isdigit():
                try:
                    size = float(word)
                except:
                    size = 10
            elif word == "large":
                size = 14
            elif word == "small":
                size = 9
            elif word[0:7] == "family:":
                family = word[7:]
        fd = pango.FontDescription(family+' '+style+' '+str(size))
        if scale != 1000:
            fd.set_size(fd.get_size() * scale / 1000)
        return fd

    def get_colours(self, attrs):
        "Return a foreground and a background colour - background might be None"
        fg = None
        bg = None
        inv = False
        for word in attrs.split(','):
            if word[0:3] == "fg:":
                fg = word[3:]
            if word[0:3] == "bg:":
                bg = word[3:]
            if word == "inverse":
                inv = True
        cmap = self.text.get_colormap()
        if inv:
            fg,bg = bg,fg
            if fg is None:
                fg = "white"
            if bg is None:
                bg = "black"
        else:
            if fg is None:
                fg = "black"

        if fg:
            try:
                c = gtk.gdk.color_parse(fg)
            except:
                c = gtk.gdk.color_parse("black")
            fgc = cmap.alloc_color(c)
        else:
            fgc = None

        if bg:
            try:
                c = gtk.gdk.color_parse(bg)
            except:
                c = gtk.gdk.color_parse("white")
            bgc = cmap.alloc_color(c)
        else:
            bgc = None

        return fgc, bgc

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
        if self.need_refresh:
            self.pane.refresh()
            self.need_refresh = False
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
        if self.pane.w == alloc.width and self.pane.h == alloc.height:
            return None
        self.pane.w = alloc.width
        self.pane.h = alloc.height
        self.need_refresh = True
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
        self.pane.call("Mouse-event", "Click-1", self.pane, (x,y))
        self.pane.refresh()

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
                if event.state & gtk.gdk.CONTROL_MASK:
                    s = "C-" + s;
        if event.state & gtk.gdk.MOD1_MASK:
            s = "M-" + s;
        self.pane.call("Keystroke", self.pane, s)
        self.pane.refresh()

    def do_clear(self, pm, colour):

        t = self.text
        if not self.bg:
            self.bg = t.window.new_gc()
        self.bg.set_foreground(colour)
        (w,h) = pm.get_size()
        pm.draw_rectangle(self.bg, True, 0, 0, w, h)

def new_display(key, home, comm2, **a):
    disp = EdDisplay(a['focus'])
    comm2('callback', disp.pane)
    return 1

class events:
    def __init__(self):
        self.active = True

    def read(self, key, focus, comm2, numeric, **a):
        self.active = True
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

editor.call("global-set-command", pane, "attach-display-pygtk", new_display);
editor.call("global-set-command", pane, "pygtkevent:activate", events_activate);
