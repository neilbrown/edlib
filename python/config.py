# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2020 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING

# This is a "config" script.  It just for experimenting
# with simple configuration.  I plan to write a proper
# config module with a more abstract language one day.
# But I want some simple configuration NOW

def config_appeared(key, focus, **a):

    p = focus['filename']

    if p and ("COMMIT_EDITMSG" in p or "/.stgit" in p):
        focus.call("doc:set:view-default", "textfill,whitespace,autospell")
        focus.call("doc:set:fill-width", "72")
        if "/git/lustre-release/" in p:
            # looks like a lustre commit, need to limit line width
            focus.call("doc:set:fill-width", "70")
            focus.call("doc:set:whitespace-width", "60")
    return edlib.Efallthrough


editor.call("global-set-command", "doc:appeared-config", config_appeared)

# Some modules I want auto-loaded.
editor.call("global-load-module", "lib-mergeview")
editor.call("global-load-module", "render-calc")
editor.call("global-load-module", "lib-compose-email")
editor.call("global-load-module", "lib-autospell")
editor.call("global-load-module", "lib-whitespace")
editor.call("global-load-module", "display-pygtk")
