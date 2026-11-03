# A Time subclass instance in a mixed Array or a poly slot is still its
# class: its own methods dispatch on it, Time's methods read its Time, and
# p and puts show it as a Time. Overrides reach Time's method through
# super, as tzinfo's TimeWithOffset does.
class Offset
  attr_reader :seconds, :abbr
  def initialize(seconds, abbr)
    @seconds = seconds
    @abbr = abbr
  end
end

class TimeWithOffset < Time
  attr_reader :timezone_offset

  def set_timezone_offset(offset)
    localtime(offset.seconds)
    @timezone_offset = offset
    self
  end

  def zone
    to = timezone_offset
    to ? to.abbr : super
  end

  def getlocal(*args)
    result = super
    result.clear_timezone_offset if result.kind_of?(TimeWithOffset)
    result
  end

  def clear_timezone_offset
    @timezone_offset = nil
    self
  end
end

t = TimeWithOffset.at(1767290645).set_timezone_offset(Offset.new(32400, "JST"))
p t.hour, t.zone, t.utc_offset
p t.kind_of?(TimeWithOffset), t.kind_of?(Time), t.instance_of?(Time)
g = t.getlocal("+00:00")
p g.class, g.timezone_offset, g.hour
p t.strftime("%H:%M %Z")
p t
puts t
items = [t, Time.at(0).utc, "x"]
items.each do |i|
  if i.is_a?(TimeWithOffset)
    puts "own #{i.zone}"
  elsif i.is_a?(Time)
    puts "time #{i.year}"
  else
    puts "other #{i}"
  end
end
p items.map(&:class)
def describe(v)
  v.respond_to?(:timezone_offset) ? "offset #{v.timezone_offset.abbr}" : "plain"
end
p describe(t), describe(Time.at(0))

# A display override of the subclass's own wins on a boxed instance too.
class Shown < Time
  def to_s
    "shown #{year}"
  end

  def inspect
    "#<Shown #{year}>"
  end
end
shown = [Shown.at(0).utc, "x"]
puts shown[0]
p shown[0]
puts "#{shown[0]}"
p shown
class Line < Time
  def to_s
    "stamp\n"
  end
end
lines = [Line.at(0), "x"]
puts lines[0]
puts "end"
