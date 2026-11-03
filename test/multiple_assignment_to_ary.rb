# A multiple assignment from one object converts it through its to_ary,
# a private one too; one whose to_ary answers nil takes the object whole.
# The assignment's own value is still the object, and a nested target
# converts its element too. nil answers no to_ary; an object whose class
# defines to_ary only later is taken whole until then. The targets'
# receivers run before the value and its to_ary.

class Pair
  def initialize(a, b) = (@a = a; @b = b)
  def to_ary = [@a, @b]
end
class Triple
  def to_ary = [1, 2, 3]
end
class Hidden
  private
  def to_ary = ["h", "i"]
end
class Opaque
  def to_ary = nil
end

a, b = Pair.new("x", "y")
puts a + b
first, *rest = Triple.new
p first, rest
x, (y, z) = Triple.new
p [x, y, z]
s, t = Hidden.new
puts s + t
o, q = Opaque.new
p o.class, q
def pair = Pair.new(3, 4)
m, n = pair
p m * n
r = (u, v = Pair.new(5, 6))
p r.class, u, v
p((i, j = Pair.new(11, 12)).class)
p [i, j]
c, (d, e) = 1, Pair.new(7, 8)
p [c, d, e]
f, (g, h) = [2, Pair.new(9, 10)]
p [f, g, h]

o = ARGV.size > 5 ? Pair.new(1, 2) : nil
i, j = o
p [i, j]
r = (i, j = o)
p r, i, j
def halves(x) = (k, l = x; [k, l])
p halves(nil), halves(Pair.new(13, 14))

class Late; end
q, w = Late.new
p [q.class, w]
class Late
  def to_ary = [:late, :now]
end
q, w = Late.new
p [q, w]

$log = []
class Box
  attr_accessor :v
end
class Logged
  def to_ary = ($log << :to_ary; [15, 16])
end
def box(tag) = ($log << tag; Box.new)
def logged = ($log << :rhs; Logged.new)
r = (box(:recv1).v, box(:recv2).v = logged)
p r.class, $log

def risky = raise("no")
class Skipped; end
begin
  risky
  class Skipped
    def to_ary = [:never, :ran]
  end
rescue
end
q, w = Skipped.new
p [q.class, w]

class Dropped
  def to_ary = [:gone, :later]
end
# an undef of to_ary that runs later: the assignment before it does not
# raise (w is :later in CRuby; the conversion is not made while the class
# holds an undef of to_ary, so w is not printed)
begin
  q, w = Dropped.new
  p :assigned
rescue NoMethodError
  p :raised
end
class Dropped
  undef_method :to_ary
end
q, w = Dropped.new
p [q.class, w]
# a nested target after a rest takes nil where the elements stop at the
# left targets, and the left target keeps its object
a, b, *r, (c, d) = 1, Pair.new(1, 2)
p [a, b.class, r, c, d]
a, *r, (c, d) = 1, Pair.new(3, 4)
p [a, r, c, d]
# a nested assignment whose value is read answers its elements as they
# were, each evaluated once, while the nested target takes the converted one
r = (a, (c, d) = 1, Pair.new(5, 6))
p [r[1].class, a, c, d]
def nested_value(o) = ((x, (y, z) = 7, o))
p nested_value(Pair.new(8, 9)).map(&:class)
