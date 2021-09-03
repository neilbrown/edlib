# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The html-to-text function extracts text from the given child,
# converts it from html to markdown, and creates a text pane with the
# markdown text.

import html2text

def html_to_text(key, home, focus, comm2, **a):
    html = focus.call("doc:get-str", ret='str')
    if not html:
        return edlib.Efail

    h = html2text.HTML2Text()
    h.inline_links = False
    h.wrap_links = False
    # automatic links get wrapped despite above,
    # so don't use them.
    h.use_automatic_links = False
    h.ul_style_dash = True
    h.body_width = 80
    h.mark_code = True
    try:
        h.pad_tables = True
        content = h.handle(html)
    except IndexError:
        # if table padding doesn't work, then don't bother at all
        # Maybe bypass_tables could be used if I put enough smarts
        # in a viewer
        h.pad_tables = False
        h.ignore_tables = True
        content = h.handle(html)

    doc = focus.call("doc:from-text", "html-document", content, ret='pane')
    comm2("cb", doc)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "html-to-text", html_to_text)
