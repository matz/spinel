# A builtin `new` whose chain a later prepend may choose, each chain ending in an empty initialize and only the prepended
# forward raising for a `**h` key it does not name: where the load record proves the prepend made before the call runs,
# that chain is the one that runs, and the call makes its check.
class Array
  def initialize(*) = nil
end
module Sized
  def initialize(*a, size: 1) = super
end
def mk(h) = Array.new(2, **h)
Array.prepend(Sized)
p((mk({extra: 1}) rescue $!.message))
p mk({size: 3})
p mk({})
h = {extra: 1}
p((Array.new(2, **h) rescue $!.message))
p Array.new(2, **{})
