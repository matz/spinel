# A Time parameter changed in place by #localtime and captured by a lambda
# that outlives the call cannot be lent the caller's slot: refused.
# spinel: reject-time-inplace: a block or lambda that can outlive the call captures it
def m(t)
  t.localtime(3600)
  -> { t.utc_offset }
end

a = Time.utc(2026, 10, 15, 1, 0, 0)
f = m(a)
p a.utc_offset
p f.call
