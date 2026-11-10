# A `&.` call whose receiver is not nil checks the argument count as a plain
# call does; a nil receiver answers nil before any count is looked at.

def t
  p yield
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end

class Loc
  def initialize(s) = @s = s
  def slice = @s
  def one(a) = a
end

# a boxed receiver: a program class and builtin classes
xs = [Loc.new("l"), "str", [1, 2], { a: 1 }, nil, 5]
zs = xs - [xs[0]]
zs.each { |x| t { x&.size(1) } }
ys = xs - [5]
ys.each { |x| t { x&.slice } }
t { xs[0]&.one }

# a typed receiver
s = "abc"
a = [1, 2, 3]
h = { k: 1 }
t { s&.size(1) }
t { s&.slice }
t { s&.slice(1) }
t { s&.upcase }
t { s&.each_char(1) { |c| c } }
t { a&.first(1, 2) }
t { a&.size(1) }
t { h&.size(1) }
t { h&.fetch }

# a receiver that can be nil: an Integer, a Float, a String, an Array
def maybe(i) = i.odd? ? nil : i
n = maybe(3)
m = maybe(4)
t { n&.succ(1) }
t { m&.succ(1) }
t { m&.succ }
f = maybe(2) && 1.5
t { f&.floor(1, 2) }
ns = maybe(2) && "x"
t { ns&.center }
na = maybe(2) && [1]
t { na&.first(1, 2) }
nn = maybe(1) && [1]
t { nn&.first(1, 2) }
t { nn&.size(1) }

# a nil receiver answers nil whatever the count; a right count is unchanged
t { nil&.size(1) }
t { nil&.slice }
t { s&.size }
t { a&.first(2) }
t { m&.succ }

# a block-taking name with a wrong count
t { a&.each(1) { |v| v } }
t { a&.map(1) { |v| v } }
t { a&.map { |v| v + 1 } }
t { a&.each { |v| v } }
t { nil&.each(1) { |v| v } }

# a program class's method with a wrong count
o = Loc.new("q")
t { o&.one }
t { o&.one(1, 2) }
t { o&.slice(1) }
t { o&.one(1) }
no = maybe(1) && o
t { no&.one }
ok = maybe(2) && o
t { ok&.one }

# the receiver runs once, then the arguments in order, then the count is
# checked; a nil receiver runs no argument
def r; puts "r"; "abc"; end
t { r&.size(1) }
def ra; puts "ra"; [1, 2]; end
t { ra&.first(1, 2) }
t { s&.size(puts("arg")) }
t { a&.size(puts("arg2")) }
t { ns&.size(puts("arg")) }
t { nn&.first(puts("a"), puts("b")) }
t { nil&.size(puts("c")) }
