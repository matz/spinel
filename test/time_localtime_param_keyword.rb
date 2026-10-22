# A keyword parameter changed in place by #localtime is lent the caller's
# Time slot like a positional one.
def shift(by: 0, t:) = t.localtime(by)

a = Time.utc(2026, 10, 15, 1, 0, 0)
shift(t: a, by: 3600)
p [a.utc_offset, a.hour]
