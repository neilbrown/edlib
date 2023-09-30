# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# Various support for working with git repos.
#
# Maintain a list of known git repos.  These can be configured, or
# added when a directory containing ".git" is added.  These are simply
# directory docs (found by docs:foreach) with a "git-root" attribute set.
#
# Provide selection-menu item which treats selection as a git hash,
# finds a repo which knowns of that hash, and shows it in a popup.
#

from edlib import *
from subprocess import *
import os.path
import re

def git_appeared(key, focus, **a):
    t = focus['doc-type']
    if not t or t != 'dir':
        return Efallthrough
    f = focus['filename']
    if not f:
        return Efallthrough
    p = os.path.join(f, ".git")
    if os.path.isdir(p):
        focus['git-root'] = "yes"
        return Efallthrough
    if os.path.isfile(p):
        try:
            f = open(p)
        except:
            return Efallthrough
        l = f.readlines()
        if l and len(l) >= 1 and l[0].startswith("gitdir: "):
            focus['git-root'] = l[0][8:].strip()

    return Efallthrough

def git_selection_menu(key, focus, **a):
    focus.call("menu-add", "Git-view", " git:view-selected-commit")
    return Efallthrough

def git_view_commit(key, focus, mark, **a):
    pt,mk = focus.call("doc:point", ret='marks')

    if not pt or not mk:
        return 1
    focus.call("selection:claim")
    focus.call("selection:discard")
    cm = focus.call("doc:get-str", pt, mk, ret='str')
    if not cm:
        return 1;
    if not re.match("g?[0-9a-fA-F]{5,}$", cm):
        if '\n' in cm:
            focus.call("Message:modal",
                       "multi-line selection is not a valid git hash")
        else:
            focus.call("Message:modal", "\"%s\" is not a valid git hash" % cm)
        return 1
    if cm[0] == 'g':
        cm = cm[1:]

    choice = []
    def choose(choice, a):
        focus = a['focus']
        root = focus['git-root']
        if not root:
            return 1
        if root == "yes":
            root = os.path.join(focus['filename'], ".git")
        env = os.environ.copy()
        env['GIT_DIR'] = root
        p = Popen(["/usr/bin/git", "describe", "--always", cm], env=env,
                         stdin=DEVNULL, stdout=DEVNULL, stderr=DEVNULL)
        if not p:
            return 1
        try:
            p.communicate(timeout=5)
        except TimeoutExpired:
            p.kill()
            p.communicate()
            return 1
        if p.returncode != 0:
            return 1
        choice.append(focus)
        # only need one - stop now
        return False
    focus.call("docs:byeach", lambda key,**a:choose(choice, a))
    if len(choice):
        pop = focus.call("PopupTile", "DM3sta", ret='pane')
        if not pop:
            focus.call("Message:modal", "popup failed")
            return 1
        doc = focus.call("doc:from-text", "*GIT view*", "", ret='pane')
        if not doc:
            pop.call("popup:close")
            focus.call("Message:modal", "doc:from-text failed")
            return 1
        dir = choice[0]['filename']
        doc.call("doc:replace", "In GIT repository: %s\n" % dir);
        doc.call("attach-shellcmd", 2, "git show "+cm, dir)
        doc['view-default'] = "diff"
        doc.call("doc:attach-view", pop, 1)
        focus.call("Message:modal", "Commit %s found in %s" % (cm, dir))
    else:
        focus.call("Message:modal", "Cannot find git commit " + cm)
    return 1

editor.call("global-set-command", "doc:appeared-git", git_appeared)
editor.call("global-set-command", "selection-menu:add-02-git",
                  git_selection_menu)
editor.call("global-set-command", "git:view-selected-commit", git_view_commit)
