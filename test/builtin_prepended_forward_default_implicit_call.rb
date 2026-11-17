# A default a prepended forward fills runs after the call's operands, and an operator's implicit call is code too: a
# default `2000 << $shift` runs $shift's to_int, which may change what an operand read reads, and the call passes the
# value read before it.
class Shift
  def to_int
    $zone.z = "+0#{($zone.z[2].to_i + 1) % 10}:00"
    0
  end
end
$shift = Shift.new
module Year
  def initialize(y = 2000 << $shift, **kw) = super
end
Time.prepend(Year)
class Zone
  attr_accessor :z
  def initialize = @z = "+00:00"
  def at = Time.new(in: @z).utc_offset
  def both = [Time.new(in: @z).utc_offset, Time.new(in: @z).utc_offset]
  def year = [Time.new(in: @z).year, @z]
end
$zone = Zone.new
p $zone.at
p $zone.z
p $zone.both
p Time.new(in: $zone.z).utc_offset
p $zone.year
p 2000 << $shift
p $zone.z
