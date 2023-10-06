# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# Create an image document from a string.  This will typicially
# be displayed with render-imageview.
#

from edlib import *
import qrcode
import io
import base64

def make_qrcode_doc(focus, str1):
    if not str1:
        return Efalse
    qr = qrcode.make(str1, box_size=1)
    i = qr.get_image()
    buf = io.BytesIO(b'')
    i.save(buf, "PNG")
    d = focus.call("doc:from-text", "*qr*",
                   base64.b64encode(buf.getvalue()).decode(), ret='pane')
    d['doc-type'] = "image"
    d['render-default'] = "imageview"
    d['view-default'] = "base64"
    d['imageview:integral'] = 'yes'
    return d

def show_qr(key, focus, **a):
    pt,mk = focus.call("doc:point", ret='marks')

    if not pt or not mk:
        return 1
    focus.call("selection:claim")
    focus.call("selection:discard")
    txt = focus.call("doc:get-str", pt, mk, ret='str')
    if not txt:
        return 1;

    d = make_qrcode_doc(focus, txt)
    p = focus.call("PopupTile", "MD3tsa", ret='pane')
    d.call("doc:attach-view", p, 1)
    return 1

def qr_selection_menu(key, focus, **a):
    focus.call("menu-add", "QRCODE", " qrcode:view-selected-text")
    return Efallthrough

editor.call("global-set-command", "selection-menu:add-02-qrcode",
                  qr_selection_menu)
editor.call("global-set-command", "qrcode:view-selected-text", show_qr)
