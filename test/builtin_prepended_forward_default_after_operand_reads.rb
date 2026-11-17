# The defaults a prepended forward fills run after the call's operands: a default that changes what an operand reads
# (an ivar, an attribute, a local through a closure) does not change the value the call passed.
module Year
  def initialize(y = ($bump.call; 2000), **kw) = super
end
Time.prepend(Year)
class Zone
  attr_accessor :z
  def initialize = @z = "+00:00"
  def at = Time.new(in: @z).utc_offset
  def twice = [Time.new(in: @z).utc_offset, Time.new(in: @z).utc_offset]
end
$zone = Zone.new
z = "+00:00"
$bump = -> { $zone.z = "+0#{$zone.z[2].to_i + 1}:00"; z = $zone.z }
p $zone.at
p $zone.twice
p Time.new(in: $zone.z).utc_offset
p Time.new(in: z).utc_offset
p z
