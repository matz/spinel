# An Integer or Float that may be nil, compared where the comparison's answer
# does not depend on its value, is evaluated for effect only: its nil is no
# TypeError.
class K
  def initialize; @a = 1; end
  def ne(x) = (self != x)
  def eq(x) = (self == x)
end

def maybe_int(f) = f ? 3 : nil
def maybe_float(f) = f ? 2.5 : nil

o = K.new
p o.ne(1), o.ne(nil)
p o.eq(1), o.eq(nil)

t = Time.at(0)
x = maybe_int(false)
p t == x, t != x
y = maybe_float(false)
p t == y, t != y
p t == maybe_int(true)

h = {a: 1}
p h == x, h != x

s = "abc"
i = s.index("z")
p o != i, o == i
p :sym == i, :sym != i

# a nullable number made by a call with an effect, as an operand: it runs
# exactly once, whether it gives nil or a number
$runs = []
def counted(f) = ($runs << f; f ? 7 : nil)
def counted_f(f) = ($runs << f; f ? 1.5 : nil)
p o == counted(true), o != counted(false), t == counted_f(false), :s != counted(true)
p o.equal?(counted(false)), o.equal?(counted(true))
p $runs

# an object's identity against a nullable number or a boxed value: the
# receiver runs whatever the argument holds, and first
$log = []
def build = ($log << :recv; K.new)
def same(x) = build.equal?(x)
def pick(f, k) = f ? k : :sym
n = maybe_int(false)
v = pick(false, o)
p [build.equal?(n), build.equal?(v)], $log.size
p same(maybe_int(true)), same(n), same(v), $log.size
p [1].map { build.equal?(counted(false)) }, $log, $runs.size
p o.equal?(pick(true, o)), o.equal?(pick(false, o))
