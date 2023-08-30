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

import edlib

import os, fcntl
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

class w3m_pane(edlib.Pane):
    def __init__(self, focus, content, delayed):
        edlib.Pane.__init__(self, focus)
        self.doc = focus
        self.pipe = None
        self.add_notify(focus, "Close")
        self.content = content
        self.have_converting = True
        focus.call("doc:replace", 1, "(Converting content to text...)\n")
        if delayed:
            self.call("doc:request:convert-now")
        else:
            self.handle_visible("key", focus)

    def handle_visible(self, key, focus, **a):
        "handle:convert-now"

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
        self.pipe = p
        # FIXME this could block if pipe fills
        os.write(p.stdin.fileno(), self.content.encode())
        p.stdin.close()
        p.stdin = None
        fd = p.stdout.fileno()
        fcntl.fcntl(fd, fcntl.F_SETFL,
                    fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
        self.call("event:read", fd, self.read)

    def handle_close(self, key, **a):
        "handle:Close"

        if self.pipe:
            self.pipe.kill()
            self.pipe.communicate()
        return 1

    def handle_doc_close(self, key, focus, **a):
        "handle:Notify:Close"
        if focus == self.doc:
            self.doc = None
            if self.pipe:
                self.pipe.kill()
        return 1

    def read(self, key, **a):
        if not self.pipe:
            return edlib.Efalse
        try:
            r = os.read(self.pipe.stdout.fileno(), 65536)
        except IOError:
            return 1

        if not self.doc:
            return edlib.Efalse

        if r:
            if self.have_converting:
                m = edlib.Mark(self.doc)
                m2 = m.dup()
                m.step(1)
                self.have_converting = False
            else:
                m = edlib.Mark(self.doc)
                m2 = m
            self.doc.call("doc:set-ref", m2)
            self.doc.call("doc:replace", 1, r.decode('utf-8','ignore'),
                          m, m2)
            parse_halfdump(self.doc)
            return 1
        # EOF
        if not self.pipe:
            return edlib.Efalse
        out, err = self.pipe.communicate()
        self.pipe = None
        if err:
            edlib.LOG("w3m-to-text", err.decode('utf-8','ignore'))

        self.close()
        return edlib.Efalse

class w3m_content(edlib.Pane):
    def __init__(self, focus):
        edlib.Pane.__init__(self, focus)

    def handle_content(self, key, focus, mark, mark2, comm2, **a):
        "handle:doc:content"
        ctx = {}
        return self.parent(key, focus, mark, mark2,
                           lambda key, **a: self.cb(comm2, ctx, **a))

    def cb(self, callback, ctx, focus, num, mark, num2, xy, **a):
        if '<' in ctx:
            # skipping to '>'
            if num == ord('>'):
                del ctx['<']
            return 1
        if '&' in ctx:
            # collecting an entity
            if num != ord(';'):
                ctx['&'] += chr(num)
                return 1
            ent = ctx['&']
            del ctx['&']
            if ent[:2] == "#x":
                val = chr(int(ent[2:], 16))
            elif ent[:1] == "#":
                val = chr(int(ent[1:], 10))
            elif ent in entities:
                val = chr(entities[ent])
            else:
                val = "!"
            callback("content", focus, ord(val[0]), mark)
            return 1
        if num == ord('<'):
            # skip to next '>'
            ctx['<'] = True
            return 1
        if num == ord('&'):
            # collect the entity
            ctx['&'] = ""
            return 1
        return callback("content", focus, num, mark)


def html_to_w3m(key, home, focus, num, comm2, **a):
    html = focus.call("doc:get-str", ret='str')
    if not html:
        return edlib.Efail
    doc = focus.call("doc:from-text", "html-document", "", ret='pane')
    w3m_pane(doc, html, num)

    comm2("cb", w3m_content(doc))
    return 1

entities = {
    "AElig" : 0x00C6,
    "Aacute" : 0x00C1,
    "Acirc" : 0x00C2,
    "Agrave" : 0x00C0,
    "Alpha" : 0x0391,
    "Aring" : 0x00C5,
    "Atilde" : 0x00C3,
    "Auml" : 0x00C4,
    "Beta" : 0x0392,
    "Ccedil" : 0x00C7,
    "Chi" : 0x03A7,
    "Dagger" : 0x2021,
    "Delta" : 0x0394,
    "ETH" : 0x00D0,
    "Eacute" : 0x00C9,
    "Ecirc" : 0x00CA,
    "Egrave" : 0x00C8,
    "Epsilon" : 0x0395,
    "Eta" : 0x0397,
    "Euml" : 0x00CB,
    "Gamma" : 0x0393,
    "Iacute" : 0x00CD,
    "Icirc" : 0x00CE,
    "Igrave" : 0x00CC,
    "Iota" : 0x0399,
    "Iuml" : 0x00CF,
    "Kappa" : 0x039A,
    "Lambda" : 0x039B,
    "Mu" : 0x039C,
    "Ntilde" : 0x00D1,
    "Nu" : 0x039D,
    "OElig" : 0x0152,
    "Oacute" : 0x00D3,
    "Ocirc" : 0x00D4,
    "Ograve" : 0x00D2,
    "Omega" : 0x03A9,
    "Omicron" : 0x039F,
    "Oslash" : 0x00D8,
    "Otilde" : 0x00D5,
    "Ouml" : 0x00D6,
    "Phi" : 0x03A6,
    "Pi" : 0x03A0,
    "Prime" : 0x2033,
    "Psi" : 0x03A8,
    "Rho" : 0x03A1,
    "Scaron" : 0x0160,
    "Sigma" : 0x03A3,
    "Thorn" : 0x00DE,
    "Tau" : 0x03A4,
    "Theta" : 0x0398,
    "Uacute" : 0x00DA,
    "Ucirc" : 0x00DB,
    "Ugrave" : 0x00D9,
    "Upsilon" : 0x03A5,
    "Uuml" : 0x00DC,
    "Xi" : 0x039E,
    "Yacute" : 0x00DD,
    "Yuml" : 0x0178,
    "Zeta" : 0x0396,
    "aacute" : 0x00E1,
    "acirc" : 0x00E2,
    "acute" : 0x00B4,
    "aelig" : 0x00E6,
    "agrave" : 0x00E0,
    "alpha" : 0x03B1,
    "amp" : 0x0026,
    "and" : 0x2227,
    "ang" : 0x2220,
    "aring" : 0x00E5,
    "asymp" : 0x2248,
    "atilde" : 0x00E3,
    "auml" : 0x00E4,
    "bdquo" : 0x201E,
    "beta" : 0x03B2,
    "brvbar" : 0x00A6,
    "bull" : 0x2022,
    "cap" : 0x2229,
    "ccedil" : 0x00E7,
    "cedil" : 0x00B8,
    "cent" : 0x00A2,
    "chi" : 0x03C7,
    "circ" : 0x02C6,
    "clubs" : 0x2663,
    "cong" : 0x2245,
    "copy" : 0x00A9,
    "crarr" : 0x21B5,
    "cup" : 0x222A,
    "curren" : 0x00A4,
    "dArr" : 0x21D3,
    "dagger" : 0x2020,
    "darr" : 0x2193,
    "deg" : 0x00B0,
    "delta" : 0x03B4,
    "diams" : 0x2666,
    "divide" : 0x00F7,
    "eacute" : 0x00E9,
    "ecirc" : 0x00EA,
    "egrave" : 0x00E8,
    "empty" : 0x2205,
    "emsp" : 0x2003,
    "ensp" : 0x2002,
    "epsilon" : 0x03B5,
    "equiv" : 0x2261,
    "eta" : 0x03B7,
    "eth" : 0x00F0,
    "euml" : 0x00EB,
    "euro" : 0x20AC,
    "exist" : 0x2203,
    "fnof" : 0x0192,
    "forall" : 0x2200,
    "frac12" : 0x00BD,
    "frac14" : 0x00BC,
    "frac34" : 0x00BE,
    "frasl" : 0x2044,
    "gamma" : 0x03B3,
    "ge" : 0x2265,
    "gt" : 0x003E,
    "hArr" : 0x21D4,
    "harr" : 0x2194,
    "hearts" : 0x2665,
    "hellip" : 0x2026,
    "iacute" : 0x00ED,
    "icirc" : 0x00EE,
    "iexcl" : 0x00A1,
    "igrave" : 0x00EC,
    "infin" : 0x221E,
    "int" : 0x222B,
    "iota" : 0x03B9,
    "iquest" : 0x00BF,
    "isin" : 0x2208,
    "iuml" : 0x00EF,
    "kappa" : 0x03BA,
    "lArr" : 0x21D0,
    "lambda" : 0x03BB,
    "lang" : 0x2329,
    "laquo" : 0x00AB,
    "lceil" : 0x2308,
    "ldquo" : 0x201C,
    "le" : 0x2264,
    "lfloor" : 0x230A,
    "lowast" : 0x2217,
    "loz" : 0x25CA,
    "lrm" : 0x200E,
    "lsaquo" : 0x2039,
    "lsquo" : 0x2018,
    "lt" : 0x003C,
    "macr" : 0x00AF,
    "mdash" : 0x2014,
    "micro" : 0x00B5,
    "middot" : 0x00B7,
    "minus" : 0x2212,
    "mu" : 0x03BC,
    "nabla" : 0x2207,
    "nbsp" : 0x00A0,
    "ndash" : 0x2013,
    "ne" : 0x2260,
    "ni" : 0x220B,
    "not" : 0x00AC,
    "notin" : 0x2209,
    "nsub" : 0x2284,
    "ntilde" : 0x00F1,
    "nu" : 0x03BD,
    "oacute" : 0x00F3,
    "ocirc" : 0x00F4,
    "oelig" : 0x0153,
    "ograve" : 0x00F2,
    "oline" : 0x203E,
    "omega" : 0x03C9,
    "omicron" : 0x03BF,
    "oplus" : 0x2295,
    "or" : 0x2228,
    "ordf" : 0x00AA,
    "ordm" : 0x00BA,
    "oslash" : 0x00F8,
    "otilde" : 0x00F5,
    "otimes" : 0x2297,
    "ouml" : 0x00F6,
    "para" : 0x00B6,
    "part" : 0x2202,
    "permil" : 0x2030,
    "perp" : 0x22A5,
    "phi" : 0x03C6,
    "pi" : 0x03C0,
    "piv" : 0x03D6,
    "plusmn" : 0x00B1,
    "pound" : 0x00A3,
    "prime" : 0x2032,
    "prod" : 0x220F,
    "prop" : 0x221D,
    "psi" : 0x03C8,
    "quot" : 0x0022,
    "rArr" : 0x21D2,
    "radic" : 0x221A,
    "rang" : 0x232A,
    "raquo" : 0x00BB,
    "rarr" : 0x2192,
    "rceil" : 0x2309,
    "rdquo" : 0x201D,
    "reg" : 0x00AE,
    "rfloor" : 0x230B,
    "rho" : 0x03C1,
    "rlm" : 0x200F,
    "rsaquo" : 0x203A,
    "rsquo" : 0x2019,
    "sbquo" : 0x201A,
    "scaron" : 0x0161,
    "sdot" : 0x22C5,
    "sect" : 0x00A7,
    "shy" : 0x00AD,
    "sigma" : 0x03C3,
    "sigmaf" : 0x03C2,
    "sim" : 0x223C,
    "spades" : 0x2660,
    "sub" : 0x2282,
    "sube" : 0x2286,
    "sum" : 0x2211,
    "sup" : 0x2283,
    "sup1" : 0x00B9,
    "sup2" : 0x00B2,
    "sup3" : 0x00B3,
    "supe" : 0x2287,
    "szlig" : 0x00DF,
    "tau" : 0x03C4,
    "there4" : 0x2234,
    "theta" : 0x03B8,
    "thetasym" : 0x03D1,
    "thinsp" : 0x2009,
    "thorn" : 0x00FE,
    "tilde" : 0x02DC,
    "times" : 0x00D7,
    "uArr" : 0x21D1,
    "uacute" : 0x00FA,
    "uarr" : 0x2191,
    "ucirc" : 0x00FB,
    "ugrave" : 0x00F9,
    "uml" : 0x00A8,
    "upsih" : 0x03D2,
    "upsilon" : 0x03C5,
    "uuml" : 0x00FC,
    "weierp" : 0x2118,
    "xi" : 0x03BE,
    "yacute" : 0x00FD,
    "yen" : 0x00A5,
    "yuml" : 0x00FF,
    "zeta" : 0x03B6,
    "zwj" : 0x200D,
    "zwnj" : 0x200C,
    }

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
    bold = False; internal = False; imgalt = False; urltag = None
    while True:
        prev_end = m.dup()
        try:
            len = doc.call("text-search", "<[^>]*>", m)
            len -= 1
        except:
            break

        st = m.dup()
        i = 0
        while i < len:
            doc.prev(st)
            i += 1
        doc.call('doc:set-attr', 1, st, "render:hide", "1")
        sol = st.dup()
        while sol < m:
            if doc.following(sol) in [ '\n', '\v', '\f' ]:
                doc.call('doc:set-attr', 1, sol, "markup:not_eol", "1")
            doc.next(sol)
        doc.call('doc:set-attr', 1, m, "render:hide", "0")

        # We only parse entities between tags, not within them
        parse_entities(doc, prev_end, st)
        # We need to reassert attributes at the start of each affected line
        if bold or internal or imgalt or urltag:
            sol = prev_end.dup()
            while sol < st:
                if doc.next(sol) in [ '\n', '\v', '\f' ]:
                    # Found start of line - re-assert things
                    if bold:
                        doc.call("doc:set-attr", 1, m, "render:bold", "1")
                    if internal:
                        doc.call("doc:set-attr", 1, m, "render:internal", "1")
                    if imgalt:
                        doc.call("doc:set-attr", 1, m, "render:imgalt", "1")
                    if urltag:
                        doc.call("doc:set-attr", 1, m, "render:url", urltag)

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
            if url:
                url = map_entities(url)
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

def map_one_entity(e):
    if e[:2] == "#x":
        return  chr(int(e[2:], 16))
    if e[:1] == "#":
        return chr(int(e[1:], 10))
    if e in entities:
        return chr(entities[e])
    return None

def parse_entities(doc, m, end):
    while True:
        try:
            len = doc.call("text-search", "&[#A-Za-z0-9]*;", m, end)
            len -= 1
        except:
            break
        st = m.dup()
        i = 0
        while i < len:
            doc.prev(st)
            i += 1
        name = doc.call("doc:get-str", st, m, ret='str')
        ent = name[1:-1]
        char = map_one_entity(ent)
        if not char:
            char = "!" + ent
        doc.call('doc:set-attr', 1, st, "render:char", "%d:%s" % (len,char))

def map_entities(str):
    ret = ""
    while True:
        i = str.find('&')
        if i < 0:
            break
        ret += str[:i]
        str = str[i:]
        i = str.find(';')
        if i < 0:
            break;
        c = map_one_entity(str[1:i])
        if c:
            ret += c
            str = str[i+1:]
        else:
            ret += '&'
            str = str[1:]

    return ret + str

edlib.editor.call("global-set-command", "html-to-text-w3m", html_to_w3m)
