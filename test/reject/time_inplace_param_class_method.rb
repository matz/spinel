# A class method the program defines on Time can answer a Time someone
# holds, so it is not lent a temporary: the change `zone` makes would stop
# at the copy, where CRuby's reaches $last. Refused.
# spinel: reject-time-inplace: only a local variable or an instance
class Time
  def self.last = $last
end

def zone(t) = t.localtime(3600)

$last = Time.utc(2026, 10, 15, 1, 0, 0)
zone(Time.last)
p $last.utc_offset
