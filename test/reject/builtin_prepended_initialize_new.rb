# A module prepended to a builtin class, or a reopening of it, defines an
# initialize that CRuby's `new` runs. The builtin construction does not, so
# the program is refused instead of answering without the method's effect
# (the prepended initialize's `$seen` write, and the elements it adds).
module Loud
  def initialize(*a)
    $seen = a
    super
    push(:added)
  end
end
Array.prepend(Loud)
p Array.new(2, 0)
p $seen
