# Set is loaded when CRuby starts, so a reopening that comes before the
# program's `require "set"` extends the Set that is there already, and the
# require loads nothing that could undo it.
class Set
  def size = 99
end
p require("set")
p Set[1, 2].size
