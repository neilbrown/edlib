# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The doc-to-text function extracts bytes from the given child,
# converts it to text using lowriter.
# Unfortunately lowriter only reads and writes a file, not a pipe..

import subprocess
import tempfile
import os

def doc_to_text(key, home, focus, str1, comm2, **a):

    if not str1 or '.' not in str1:
        # we need a file name to get an extension
        return edlib.Enoarg;

    content = focus.call("doc:get-bytes", ret='bytes')

    ext = str1[str1.rindex('.'):]
    fd, path = tempfile.mkstemp(ext, "edlib-office-temp")
    os.write(fd, content)
    os.close(fd)
    newpath = path[:path.rindex('.')] + ".txt"

    p = subprocess.Popen(["/usr/bin/lowriter", "--convert-to", "txt:Text",
                          path], close_fds=True,
                         cwd = os.path.dirname(path),
                         stdout=subprocess.PIPE, stderr = subprocess.PIPE,
                         stdin =subprocess.DEVNULL)
    out,err = p.communicate()
    if err:
        edlib.LOG("doctotext:", err.decode("utf-8", 'ignore'))

    try:
        with open(newpath, 'rb') as fp:
            out = fp.read()
            os.unlink(newpath)
    except:
        out = (b"DOC conversion failed:\n" + err)
    os.unlink(path)

    doc = focus.call("doc:from-text", "office-document",
                         out.decode("utf-8", 'ignore'),
                         ret='pane')
    comm2("cb", doc)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "doc-to-text", doc_to_text)
