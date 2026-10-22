# An instance method's Time parameter changed in place by #localtime / #utc
# is lent the caller's slot like a top-level method's: the caller sees the
# new zone, as in CRuby.
class Clock
  def to_tokyo(t) = t.localtime(32400)
  def to_utc(t)
    t.utc
    nil
  end
end

a = Time.utc(2026, 10, 15, 1, 0, 0)
Clock.new.to_tokyo(a)
p [a.utc_offset, a.hour]
Clock.new.to_utc(a)
p [a.utc?, a.hour]
