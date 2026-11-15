# A method of a module included at the top level runs on any object, main
# too, and keeps its instance variables on that object through the
# reflective reads and writes. A multiple-assignment target (`@a, @b = ...`)
# is a write those cannot take, so it is refused, naming its line. Missed by
# the check, the yielding method was inlined and the C referred to a `self`
# that does not exist at the top level (#3775).
module Pair
  def pair
    @a, @b = yield, 2
    :ok
  end
end

include Pair
p pair { 1 }
