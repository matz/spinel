# An instance's inspect shows its own ivars only, not the class's.
class P
  @origin = "class"
  def self.origin = @origin
  def initialize(x) = @x = x
  attr_reader :x
end
a = P.new(1)
puts a.inspect.sub(/0x\h+/, "X")
p a.instance_variables
p a.x
p P.origin
