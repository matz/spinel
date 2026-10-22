# Time#localtime / #utc change the receiver in place, so a caller that
# passed it sees the change (CRuby's time.rb force_zone! relies on it).
def to_tokyo(t) = t.localtime(32400)
def to_utc(t)
  t.utc
  nil
end
def nested(t) = to_tokyo(t)

a = Time.utc(2026, 10, 15, 1, 0, 0)
to_tokyo(a)
p a.utc_offset

b = Time.new(2026, 10, 15, 10, 0, 0, "+09:00")
to_utc(b)
p b.utc?

c = Time.utc(2026, 10, 15, 1, 0, 0)
nested(c)
p c.hour

d = Time.utc(2026, 10, 15, 1, 0, 0)
r = to_tokyo(d)
p [r.utc_offset, d.utc_offset]
