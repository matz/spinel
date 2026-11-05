# A `require "set"` inside a method loads nothing the first time it runs:
# Set is there from the program's start, and a reopening before the call
# keeps its method.
class Set
  def size = 99
end
def go = require("set")
p go
p Set[1, 2].size
p go
