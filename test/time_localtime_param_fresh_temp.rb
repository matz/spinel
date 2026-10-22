# A new Time passed to a parameter that #localtime changes in place has no
# other holder: it is lent a temporary, and the method's answer is the
# changed Time.
def to_tokyo(t) = t.localtime(32400)

base = Time.utc(2026, 10, 15, 1, 0, 0)
p to_tokyo(Time.utc(2026, 10, 15, 1, 0, 0)).hour
p to_tokyo(base + 3600).hour
p base.utc_offset
