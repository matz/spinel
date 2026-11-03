# A subclass of Time is a Time: its instances are made by its own class's
# `new`, `now`, `at`, `utc` and `local`, answer Time's methods, carry their
# own ivars and methods, and stand where a Time is wanted. The methods that
# change a Time in place (utc, localtime) change the instance and answer
# it; the ones that copy it (dup, getutc, getlocal) answer the subclass;
# the arithmetic and rounding answer a plain Time, as CRuby does.
class Stamp < Time
  attr_reader :label

  def initialize(year, month, day, hour, min, sec, zone)
    super(year, month, day, hour, min, sec, zone)
    @label = "stamp"
  end

  def describe
    "#{label} #{year}-#{month}"
  end
end

t = Stamp.new(2026, 1, 2, 3, 4, 5, "+09:00")
p t.class
p t.label
p t.describe
p t.is_a?(Time)
p t.year, t.month, t.day, t.hour, t.min, t.sec, t.utc_offset
p t.to_i
p (t + 60).class
p (t + 60).min
p (t - Time.at(0)).class
p t.round.class
p t.dup.class
p t.dup.label
p t.getutc.class
p t.getutc.hour
p t == Time.new(2026, 1, 2, 3, 4, 5, "+09:00")
p t < Time.new(2027, 1, 1)
p t.strftime("%Y-%m-%d %H:%M:%S %z")
u = Stamp.new(2026, 1, 2, 3, 4, 5, "+09:00")
v = u.utc
p v.equal?(u), u.class, u.hour, u.utc_offset, u.label
u.localtime("+05:00")
p u.hour, u.utc_offset
p Stamp.at(0).class
p Stamp.at(0).label
p Stamp.utc(2026, 3, 4).class
p Stamp.utc(2026, 3, 4).day
p Stamp.now.class

def year_of(time)
  time.year
end
p year_of(t)
p [t, Time.at(0)].min.class
