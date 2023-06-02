# -*- coding: utf-8 -*-
# Copyright Neil Brown ©2023 <neil@brown.name>
# May be distributed under terms of GPLv2 - see file:COPYING
#
# The ical-to-text function extracts text from the given child,
# converts it from ical to simple text, and creates a text pane with
# that text.

import edlib

import time
import icalendar

def ical_to_text(key, home, focus, comm2, **a):
    ical = focus.call("doc:get-str", ret='str')
    if not ical:
        return edlib.Efail

    try:
        c = icalendar.cal.Calendar.from_ical(ical)
    except ValueError:
        return edlib.Efail

    content = ""
    for sub in c.subcomponents:
        if type(sub) != icalendar.cal.Event:
            continue
        for k in ['summary', 'description', 'dtstart', 'dtend']:
            if k not in sub:
                continue
            v = sub[k]
            if k == 'summary':
                content += "Summary: %s\n" % v
            if k == 'description':
                v = "    " + "\n    ".join(v.split("\n"))
                content += "Description: %s\n" % v
            if k == 'dtstart':
                if len(v.to_ical()) == 8:
                    # just a date:
                    d = v.dt.strftime("%Y-%b-%d")
                else:
                    ts = v.dt.timestamp()
                    d = time.strftime("%Y-%b-%d %H:%M:%S", time.localtime(ts))
                content += "Start: %s\n" % d
            if k == 'dtend':
                if len(v.to_ical()) == 8:
                    # just a date:
                    d = v.dt.strftime("%Y-%b-%d")
                else:
                    ts = v.dt.timestamp()
                    d = time.strftime("%Y-%b-%d %H:%M:%S", time.localtime(ts))
                content += "End: %s\n" % d
        content += "\n"

    doc = focus.call("doc:from-text", "ical-document", content, ret='pane')
    comm2("cb", doc)
    return 1

edlib.editor.call("global-set-command", "ical-to-text", ical_to_text)
