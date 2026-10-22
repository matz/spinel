# A Time parameter changed in place by #localtime and then assigned cannot
# be the caller's slot (the assignment would reach the caller too) and has
# no shared form: refused, where it used to drop the change silently.
# spinel: reject-time-inplace: the method also assigns the parameter
def m(t)
  t.localtime(3600)
  t = Time.utc(2000, 1, 1)
  t.hour
end

a = Time.utc(2026, 10, 15, 1, 0, 0)
p m(a)
p a.utc_offset
