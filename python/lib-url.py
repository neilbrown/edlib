# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# lib-url support highlighting and activating urls in documents.
# A global "url:mark-up" finds urls in a document and marks them
# with "render:url" attribute.  value is length:tag.
# The "tag" leads to a pane attribute 'url:tag' which hold the full url.
#
# "render:url-view" is an overlay pane which:
#  - responds to map-attr for render:url, adding the active-tag attr
#  - handles Activate:url to also activate the url

import edlib

def mark_urls(key, focus, mark, mark2, **a):
    doc = focus
    ms = mark
    if not ms:
        ms = edlib.Mark(doc)
    me = mark2
    if not me:
        me = ms.dup()
    doc.call("doc:set-ref", me)
    while ms < me:
        try:
            len = doc.call("text-search",
                            "(http|https|ftp|mail):[^][\\s\";<>]+", ms, me)
            len -= 1
        except:
            return
        # People sometimes put a period or ')' at the end of a URL.
        while doc.prior(ms) in '.)':
            doc.prev(ms)
            len -= 1
        m1 = ms.dup()
        i = 0
        while i < len:
            doc.prev(m1)
            i += 1
        url = doc.call("doc:get-str", m1, ms, ret='str')
        tag = doc['next-url-tag']
        if not tag:
            tag = "1"
        doc.call("doc:set-attr", 1, m1, "render:url", "%d:%s"%(len,tag))
        doc['next-url-tag'] = "%d" % (int(tag) + 1)
        doc["url:" + tag] = url

def attach_url(key, focus, comm2, **a):
    p = url_view(focus)
    if comm2:
        comm2("cb", p)
    return 1

class url_view(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_map_attr(self, key, focus, mark, str1, str2, comm2, **a):
        "handle:map-attr"
        if str1.startswith("render:url"):
            w = str2.split(':')
            if len(w) == 2:
                tg = w[1]
                leng = int(w[0])
            else:
                tg = str2
                leng = 100000
            if str1 == "render:url-end":
                leng = -1
            comm2("attr:callback", focus, leng, mark,
                  "fg:cyan-60,underline,active-tag:url,url-tag="+tg, 120)
            return 1

    def handle_click(self, key, focus, mark, str2, **a):
        "handle:Activate:url"
        a = str2.split(',')
        tag=""
        for w in a:
            if w.startswith("url-tag="):
                tag = w[8:]
        if not tag:
            return 1
        # might be in a multipart
        url = focus.call("doc:get-attr", mark,
                         "multipart-this:url:" + tag,
                         ret='str')
        if not url:
            # or might be in main document
            url = focus["url:" + tag]
        if url:
            focus.call("Message", "Opening url [%s] <%s>" % (tag,url))
            focus.call("Display:external-viewer", url)
        else:
            focus.call("Message", "URL tag %s not found" % tag)
        return 1

edlib.editor.call("global-set-command", "url:mark-up", mark_urls)
edlib.editor.call("global-set-command", "attach-render-url-view", attach_url)
