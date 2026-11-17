# A builtin `new` whose chain a later prepend chooses at run time, each choice ending in an empty initialize, where only
# the prepended forward raises for a `**h` key it does not name: the check the call needs depends on which chain runs,
# so the call is refused rather than answered as either.
class Array
  def initialize(*) = nil
end
module Sized
  def initialize(*a, size: 1) = super
end
def mk(h) = Array.new(2, **h)
p mk({extra: 1})
Array.prepend(Sized)
p((mk({extra: 1}) rescue $!.message))
