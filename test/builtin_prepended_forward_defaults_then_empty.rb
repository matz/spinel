# A prepended forward that leaves defaults unfilled ahead of an empty
# reopened initialize: the call builds the builtin's empty value, after the
# defaults with an effect have run, a later one reading an earlier one.
$log = []
class Array
  prepend(Module.new { def initialize(n = ($log << :n; 2), f = ($log << n + 1; n)) = super })
  def initialize(*a) = nil
end
p Array.new, $log
p Array.new(5), $log
p Array.new(5, 6), $log

class String
  prepend(Module.new { def initialize(s = ($log << :s; +"x")) = super })
  def initialize(*a) = nil
end
p String.new, $log

# The empty value the call builds is the builtin's own, whatever the
# parameters of the empty initialize that dropped the arguments.
class Hash
  def initialize(a, b) = nil
end
x = 5
p Hash.new(x, x + 1), Hash.new(1, 2)[:k]
