# A top-level include inside a block the load phase runs at once
# (`1.times { include H }`) is main's include like any other: bare calls
# after it reach the module's methods.
module H
  def h = "h"
  def twice(x) = x * 2
end
1.times { include H }
puts h
p twice(21)
