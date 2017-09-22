
import notmuch

def do(m, indent):
    pf = "++" if m.is_match() else "--"
    print indent+ pf , m.get_header("From"), m.get_header("Subject")
    rl = list(m.get_replies())
    rl.sort(key = lambda ms : (ms.get_date(),ms.get_header("Subject")))
    for r in rl:
        do(r, indent + pf)
        #print "++" if r.is_match() else "--", r.get_header("Subject")

db = notmuch.Database()

q = notmuch.Query(db, "thread:0000000000005561 tag:inbox")
t = q.search_threads()
for th in t:
    print th.get_subject(), th.get_newest_date()
    for tl in th.get_toplevel_messages():
        do(tl, "")
