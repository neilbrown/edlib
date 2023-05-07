# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The html-to-text-w3m function extracts text from the given child and
# transforms it with w3m to a "half-dump" format which is simplified HTML
# which only marks up links and bold and similar which affect appearance
# of characters but not their position.
# The view-default is set to w3m-halfdump which hides the markup text and
# applied the changes to the text as render attributes.
#

import subprocess

def get_attr(tagl, tag, attr):
    # Find attr="stuff" in tag, but search for tag in tagl
    # which is a lower-cased version.
    k = tagl.find(attr+'="')
    e = -1
    if k > 0:
        e = tagl.find('"', k+len(attr)+2)
    if e > k:
        return tag[k+len(attr)+2:e]
    return None

def html_to_w3m(key, home, focus, comm2, **a):
    htmlb = focus.call("doc:get-str", ret='bytes')
    if not htmlb:
        return edlib.Efail
    html = htmlb.decode("utf-8", "ignore")
    p = subprocess.Popen(["/usr/bin/w3m", "-halfdump", "-o", "ext_halfdump=1",
                          "-I", "UTF-8", "-O", "UTF-8",
                          "-o", "display_image=off",
                          "-o", "pre_conv=1",
                          "-cols", "72",
                          "-T", "text/html"],
                         close_fds = True,
                         stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE,
                         stdin=subprocess.PIPE)
    out,err = p.communicate(html.encode())
    if err:
        edlib.LOG("w3m:", err.decode("utf-8","ignore"))
    if out:
        doc = focus.call("doc:from-text", "html-document",
                         out.decode("utf-8","ignore"),
                         ret='pane')
    else:
        doc = focus.call("doc:from-text", "html-document",
                         err.decode("utf-8","ignore"),
                         ret='pane')

    parse_halfdump(doc)
    comm2("cb", doc)
    return 1

def parse_halfdump(doc):
    # recognise and markup
    # <[Bb]> .. </b>  bold
    # <a href=....>...</a> anchor
    # <internal>...</internal> hide
    # <anything-else> - ignore
    #
    # &foo; - replace with one char:
    #   amp - &
    #   rsquo - '
    #   emsp
    #   lt  - <
    #   gt  - >
    #   #x.... utf-8 hex

    m = edlib.Mark(doc)
    bold = False; internal = False; imgalt = False; url = None
    while True:
        try:
            if bold or internal or url or imgalt:
                len = doc.call("text-search", "(^.|<[^>]*>)", m)
            else:
                len = doc.call("text-search", "<[^>]*>", m)
            len -= 1
        except:
            break
        if len == 1:
            # Found start of line - re-assert things
            if bold:
                doc.call("doc:set-attr", 1, m, "render:bold", "1")
            if internal:
                doc.call("doc:set-attr", 1, m, "render:internal", "1")
            if imgalt:
                doc.call("doc:set-attr", 1, m, "render:imgalt", "1")
            if urltag:
                doc.call("doc:set-attr", 1, m, "render:url", urltag)
            continue

        st = m.dup()
        i = 0
        while i < len:
            doc.prev(st)
            i += 1
        doc.call('doc:set-attr', 1, st, "render:hide", "%d" % len)

        tag = doc.call("doc:get-str", st, m, ret='str')
        tagl = tag.lower()
        if tagl == "<b>":
            doc.call("doc:set-attr", 1, m, "render:bold", "1")
            bold=True
        elif tagl == "</b>" and bold:
            doc.call("doc:set-attr", 1, m, "render:bold", "0")
            bold = False
        elif tagl == "<internal>":
            doc.call("doc:set-attr", 1, m, "render:internal", "1")
            internal = True
        elif tagl == "</internal>":
            doc.call("doc:set-attr", 1, m, "render:internal", "0")
            internal = False
        elif tagl[:9] == "<img_alt ":
            doc.call("doc:set-attr", 1, m, "render:imgalt", "1")
            imgalt = True
        elif tagl == "</img_alt>":
            doc.call("doc:set-attr", 1, m, "render:imgalt", "0")
            imgalt = False
        elif tagl[:3] == "<a ":
            url = get_attr(tagl, tag, "href")
            urltag = get_attr(tagl, tag, "hseq")
            if not urltag:
                urltag = doc['next-url-tag']
                if not urltag:
                    urltag = "1"
                doc['next-url-tag'] = "%d" % (int(urltag)+1)
                urltag = "i" + urltag
            urltag = "w3m-" + urltag
            if url:
                doc.call("doc:set-attr", 1, m, "render:url", urltag)
                doc["url:" + urltag] = url
        elif tagl == "</a>":
            doc.call("doc:set-attr", 1, m, "render:url-end", urltag)
            url = None; urltag = None

    m = edlib.Mark(doc)
    while True:
        try:
            len = doc.call("text-search", "&[#A-Za-z0-9]*;", m)
            len -= 1
        except:
            break
        st = m.dup()
        i = 0
        while i < len:
            doc.prev(st)
            i += 1
        name = doc.call("doc:get-str", st, m, ret='str')
        char = name[1:-1]
        if char == "amp":
            char = "&"
        elif char == "lt":
            char = "<<"
        elif char == "gt":
            char = ">"
        elif char[:2] == "#x":
            char = chr(int(char[2:], 16))
        elif char[:2] == "#":
            char = chr(int(char[1:], 10))
        elif char == "zwnj":
            char = ""
        elif char == "emsp":
            char = " "
        elif char == "rsquo":
            char = chr(8217)
        else:
            char = "!" + char
        doc.call('doc:set-attr', 1, st, "render:char", "%d:%s" % (len,char))

if "editor" in globals():
    editor.call("global-set-command", "html-to-text-w3m", html_to_w3m)
