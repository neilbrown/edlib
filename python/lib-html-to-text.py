# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The html-to-text function extracts text from the given child,
# converts it from html to markdown, and creates a text pane with the
# markdown text.

import html2text

def get_str(p):
    m = edlib.Mark(p)
    ret = ""
    c = p.next(m)
    while c:
        ret += c
        c = p.next(m)
    ret += '\n'
    return ret

def html_to_text(key, home, focus, comm2, **a):
    #html = focus.call("doc:get-str", ret='str')
    html = get_str(focus)

    h = html2text.HTML2Text()
    h.inline_links = False
    h.wrap_links = False
    h.pad_tables = False # True fails sometimes
    h.ul_style_dash = True
    h.body_width = 72
    h.mark_code = True
    content = h.handle(html)

    doc = focus.call("doc:from-text", "html-document", content, ret='focus')
    comm2("cb", doc)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "html-to-text", html_to_text)
