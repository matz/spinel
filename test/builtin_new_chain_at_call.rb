# A builtin's `new` runs the initialize chain in force where it runs: a
# reopening that comes after the call is not yet made, so the first `new`
# below builds Array's own value through the prepended forward, and the one
# after the reopening builds the empty value its initialize leaves.
module Pass
  def initialize(*a, &b) = super
end
Array.prepend(Pass)
p Array.new(2, 0)
class Array
  def initialize(*); end
end
p Array.new(2, 0)
p Array.new
