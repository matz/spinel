# spinel: int64
# String#concat and #prepend given a splat, a list whose length is known
# only at run time: every operand is converted before the receiver changes

s = +"a"
a = ["b", "c"]
r = s.concat(*a)
p s, r.equal?(s)

# an Integer is a codepoint in the receiver's encoding
s = +"a"
cp = [98, "c", 0x3b1]
s.concat(*cp)
p s.bytes, s.encoding
b = "a".b
b.concat(*[200, "b"].to_a)
p b, b.encoding

# no operands: the receiver is still checked
e = []
p (+"a").concat(*e)
f = "z".freeze
begin; f.concat(*e); rescue => x; p x.class, x.message; end
begin; f.prepend(*e); rescue => x; p x.class, x.message; end
begin; f.concat(*a); rescue => x; p x.class, x.message; end

# a bad operand raises before anything is added
s = +"a"
bad = ["b", nil]
begin; s.concat(*bad); rescue => x; p x.class, x.message; end
begin; s.prepend(*bad); rescue => x; p x.class, x.message; end
ints = ["x", 1]
begin; s.prepend(*ints); rescue => x; p x.class, x.message; end
big = [2**70]
begin; s.concat(*big); rescue => x; p x.class, x.message; end
p s

# the receiver among its own operands adds its bytes as they were
s = +"ab"
self_ops = [s, s]
s.concat(*self_ops)
p s
t = +"xy"
t.prepend(*[t, "-", t].to_a)
p t

# splats beside plain operands, and two of them
s = +"a"
c = ["c", "d"]
d = ["f"]
s.concat("b", *c, "e", *d)
t = +"z"
t.prepend("w", *c, *d)
p s, t

# a rest parameter, through several levels
def m3(s, *r) = s.concat(*r)
def m2(s, *r) = m3(s, *r)
def m1(s, *r) = m2(s, "-", *r)
def pre(s, *r) = s.prepend(*r)
p m1(+"a", "b", "c"), m1(+"a"), pre(+"v", "1", "2")

# a receiver of several classes, read at run time
cat = ->(x, *r) { x.concat(*r) }
pre2 = ->(x, *r) { x.prepend(*r) }
p cat.call(+"a", "b", 99), cat.call([1], [2], [3])
p pre2.call(+"a", "b", "c"), pre2.call([1], 2, 3)

# an object's #to_str, and a #to_str that raises part way
class ToS
  def to_str = "T"
end
class Bad
  def to_str
    raise "boom" if $boom
    "never"
  end
end
$boom = true
s = +"a"
objs = [ToS.new, "b"]
s.concat(*objs)
late = ["q", Bad.new]
begin; s.concat(*late); rescue => x; p x.message; end
p s

# a String subclass answers itself
class MyS < String; end
ms = MyS.new("a")
r = ms.concat(*a)
p r, r.class, r.equal?(ms)

# an ivar and a global receiver, two calls in one statement
class Holder
  def initialize = @s = +"i"
  def add(xs) = @s.concat(*xs)
end
p Holder.new.add(["j", "k"])
$g = +"g"
$g.concat(*a)
s = +"a"
t = +"b"
p(s.concat(*a), t.prepend(*a), $g)

# a block the builtin never calls: a literal, a Proc local, an empty `**`
def l(*a) = a
pr = proc { }
e = {}
s = +"a"
p s.concat(*l("b"), &pr), s.prepend(*l("z"), &pr), s.concat(*l("c"), **e)
p s.concat(*l("d")) { }, s&.prepend(*l("y")) { }
xs = [+"ab", [1]]
x = xs[0]
y = xs[1]
p x.concat(*l("c"), &pr), y.concat(*l([2]), &pr), x.concat(*l("w"), **e), y.concat(*l([3]), **e)
p x.concat(*l("v")) { }, y.concat(*l([4])) { }
p y.push(*l(4), **e), y.unshift(*l(0), **e), y.insert(1, *l(9), **e), y.push(*l(5), **{ k: 1 })
begin; s.concat(*l("b"), **{ k: 1 }); rescue TypeError => e; p e.message; end

# a `&` value that cannot become a block raises once the operands have run,
# on a `&.` call too; nil passes no block
bad = [5, proc { }][0]
none = [nil, 1][0]
s = +"a"
begin; s.concat(*l("b"), &bad); rescue TypeError => e; p e.message; end
begin; s&.prepend(*l("b"), &bad); rescue TypeError => e; p e.message; end
begin; xs[0].concat(*l("b"), &bad); rescue TypeError => e; p e.message; end
p s.concat(*l("b"), &none), s&.concat(*l("c"), &none), none&.concat(*l("d"), &bad)
