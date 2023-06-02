# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2020-2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# This is a "config" script.  It just for experimenting
# with simple configuration.  I plan to write a proper
# config module with a more abstract language one day.
# But I want some simple configuration NOW

from edlib import editor
import os

def config_appeared(key, focus, **a):

    p = focus['filename']

    if p and ("COMMIT_EDITMSG" in p or "/.stgit" in p):
        focus.call("doc:append:view-default", ",textfill,whitespace,autospell")
        focus.call("doc:set:fill-width", "72")
        if "/git/lustre-release/" in p:
            # looks like a lustre commit, need to limit line width
            focus.call("doc:set:fill-width", "70")
            focus.call("doc:set:whitespace-width", "60")

    if p and p[-3:] == ".md":
        # Until I have a real markdown module, I need this at least.
        if os.getenv("EDLIB_TESTING"):
            focus.call("doc:append:view-default", ",textfill,whitespace")
        else:
            focus.call("doc:append:view-default", ",textfill,whitespace,autospell")
        focus["fill-width"] = "72"
        focus["word-wrap"] = "1"
        focus["fill:start-re"] = ("^("
                                  "[^a-zA-Z0-9\\n]*$|" # empty/puctuation line
                                  " *-|"               # list item
                                  " *- *\\[[ X]]|"     # todo list item
                                  " *#+|"              # section head
                                  " *[0-9]*\\.)")      # Numbered list

editor.call("global-set-command", "doc:appeared-config", config_appeared)

# Some modules I want auto-loaded.
editor.call("global-load-module", "lib-mergeview")
editor.call("global-load-module", "render-calc")
editor.call("global-load-module", "lib-compose-email")
editor.call("global-load-module", "lib-autospell")
editor.call("global-load-module", "lib-whitespace")
editor.call("global-load-module", "display-pygtk")
editor.call("global-load-module", "display-x11-xcb")
editor.call("global-load-module", "lib-x11selection-xcb")

if 'HOME' in os.environ:
    path = os.environ['HOME'] + "/.config/edlib/config.py"
try:
    with open(path, 'r') as fp:
        out = fp.read()
        exec(out)
except:
    pass
