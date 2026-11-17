# A reopening of a builtin class whose initialize has an empty body (or a
# bare nil) is the initialize CRuby's `new` runs: the builtin's own does not,
# so the value is the empty one whatever the call passes. The arguments and a
# block are only received.
class Array
  def initialize(*); end
end
class Hash
  private def initialize(*) = nil
end
p Array.new
p Array.new(2, 0)
p Array.new(3) { |i| i * 2 }
p Hash.new(5)[:x]
p Hash.new(5).size
a = Array.new(2, :x)
a << 1
p a
# An included module's initialize sits behind the builtin's own and never runs.
module Behind
  def initialize(*a)
    $ran = true
    super
  end
end
class String
  include Behind
end
p String.new("ab")
p $ran

# The empty method only receives its arguments and block pass: they are
# still evaluated, in CRuby's order, and their effects stay.
$log = []
def arg(v)
  $log << v
  v
end
def blk
  $log << :blk
  :to_s.to_proc
end
p Array.new(arg(2), arg(0), &blk)
p Hash.new(&blk)[:a]
p $log
