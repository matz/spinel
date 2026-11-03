# Time's makers on a Class value held in a variable: the class a caller
# passes, Time or a subclass of Time, makes the Time (tzinfo's
# `def new_time(klass = Time) = klass.at(@value, @sub_second * 1_000_000)`,
# which its TimestampWithOffset#to_time calls with TimeWithOffset). Any
# other class value raises NoMethodError, as CRuby does.
class TimeWithOffset < Time
  attr_reader :abbr

  def set_abbr(abbr)
    @abbr = abbr
    self
  end
end

class Timestamp
  def initialize(value, sub_second)
    @value = value
    @sub_second = sub_second
  end

  def to_time
    new_time
  end

  def to_offset_time(abbr)
    new_time(TimeWithOffset).set_abbr(abbr)
  end

  private

  def new_time(klass = Time)
    klass.at(@value, @sub_second * 1_000_000)
  end
end

ts = Timestamp.new(1767290645, 0.5)
t = ts.to_time
p t.class, t.to_i, t.usec
o = ts.to_offset_time("JST")
p o.class, o.to_i, o.usec, o.abbr, o.is_a?(Time)

def now_of(klass)
  klass.now
end
p now_of(Time).class
p now_of(TimeWithOffset).class
k = Time
p k.at(0).to_i
k = String
begin
  k.at(0)
rescue NoMethodError => e
  p e.message
end
