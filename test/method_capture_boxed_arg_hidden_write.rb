# A Method local that one plain write names a method for, and that takes
# another Method some other way: a multiple assignment from a list or from
# an Array, `&&=`, or a parameter's argument. The call runs the Method the
# local holds, with the boxed argument of method_capture_boxed_arg.rb. The
# local was taken to hold only the method its plain write names, and the
# other was called through that one's signature: its Integer parameter
# read the boxed value unchecked, and `second` answered 2 for 1.5.
module Listed
  def self.first(value) = [:first, value]
  def self.second(value) = [:second, value]
end
def listed_second = Listed.method(:second)
def listed(value)
  chosen = Listed.method(:first)
  spare, chosen = 0, listed_second
  chosen.call(value)
end
module Spread
  def self.first(value) = [:first, value]
  def self.second(value) = [:second, value]
end
def spread_second = Spread.method(:second)
def spread(value)
  chosen = Spread.method(:first)
  spare, chosen = [0, spread_second]
  chosen.call(value)
end
module Anded
  def self.first(value) = [:first, value]
  def self.second(value) = [:second, value]
end
def anded_second = Anded.method(:second)
def anded(value)
  chosen = Anded.method(:first)
  chosen &&= anded_second
  chosen.call(value)
end
module Passed
  def self.first(value) = [:first, value]
  def self.second(value) = [:second, value]
end
def passed_second = Passed.method(:second)
def passed(chosen, value, flag)
  chosen = Passed.method(:first) if flag
  chosen.call(value)
end
p Listed.first(0), Listed.second(0), Spread.first(0), Spread.second(0)
p Anded.first(0), Anded.second(0), Passed.first(0), Passed.second(0)
[1.5, "str", nil, [2, 3], 4].each do |value|
  p listed(value), spread(value), anded(value)
  p passed(passed_second, value, false), passed(passed_second, value, true)
end
captured = 5
runner = lambda do |value|
  chosen = Listed.method(:first)
  spare, chosen = [captured, listed_second]
  -> { chosen.call(value) }.call
end
p runner.call(2.5), runner.call(:sym)
