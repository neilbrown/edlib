# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2021 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The pdf-to-text function extracts text from the given child,
# converts it from pdf to text, and creates a text doc with the
# text.

import subprocess

def pdf_to_text(key, home, focus, comm2, **a):
    pdf = focus.call("doc:get-bytes", ret='bytes')

    p = subprocess.Popen(["/usr/bin/pdftotext", "-layout", "-", "-"], close_fds=True,
                         stdout=subprocess.PIPE, stderr = subprocess.PIPE,
                         stdin =subprocess.PIPE)
    out,err = p.communicate(pdf)
    if err:
        edlib.LOG("pdftotext:", err.decode("utf-8"))

    if out:
        doc = focus.call("doc:from-text", "pdf-document", out.decode("utf-8"),
                         ret='focus')
    else:
        doc = focus.call("doc:from-text", "pdf-document",
                         "PDF conversion failed\n" + err.decode(),
                         ret='focus')
    comm2("cb", doc)
    return 1

if "editor" in globals():
    editor.call("global-set-command", "pdf-to-text", pdf_to_text)
