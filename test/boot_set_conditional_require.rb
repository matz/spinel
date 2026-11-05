# A `require "set"` that does not run, then a reopening of Set and a
# `require "set"` that does: Set is loaded from the start, so the reopening
# is the last definition of the method however the requires go.
require "set" if ARGV.size > 5
class Set
  def size = 77
end
p Set[1, 2].size
p require("set")
p Set[1, 2].size
