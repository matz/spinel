# A module prepended to a builtin class whose initialize only calls `super`
# with what it received runs the builtin's own initialize, so `new` builds the
# value it always did, through one prepend or two.
module Pass1
  def initialize(*a, &b) = super
end
module Pass2
  def initialize(*a, &b)
    super
  end
end
Array.prepend(Pass1)
Array.prepend(Pass2)
p Array.new(2, 0)
p Array.new(3) { |i| i * 2 }
