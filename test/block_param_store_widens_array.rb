# A block parameter that stores another kind into the Array a yield or a
# block's call binds it to widens that Array where it is built, whichever
# parameter binds it (a required, an optional, a post, a keyword) and
# however many methods hand the block on.

def y1(a) = yield(a)
def y2(a, b) = yield(a, b)
def yk(a) = yield(k: a)
def call1(a, &b) = b.call(a)
def fwd1(a, &b) = call1(a, &b)
def fwd2(a, &b) = fwd1(a, &b)

a = [+"s"]; y1(a) { |x| x << 1 }; p a
a = [+"s"]; y1(a) { |x| x.push(2.5) }; p a
a = [1]; y1(a) { |x| x.concat(["t"]) }; p a
a = [1]; y1(a) { |x| x.insert(0, :q) }; p a
a = [1]; y1(a) { |x| x[1] = "u" }; p a
a = [1.5]; y1(a) { |x| x.unshift(nil) }; p a
a = [+"s"]; y1(a) { |x = nil| x << 3 }; p a
a = [+"s"]; y2(0, a) { |i, x = nil| x << i }; p a
a = [+"s"]; y2(0, a) { |i, *r, x| x << 4 }; p a
a = [+"s"]; yk(a) { |k:| k << 5 }; p a
a = [+"s"]; y1(a) { |x, k: 6| x << k }; p a
a = [+"s"]; fwd2(a) { |x| x << 7 }; p a
a = [+"s"]; call1(a) { |x| x << 8; p x.last + 1; p x[0].upcase }; p a

# one Array reaching the block twice, and a value that may be nil
b = [+"t"]
y1(b) { |x| x << 9 }
y1(b) { |x| x << :z }
p b
c = b.size > 2 ? [+"n"] : nil
y1(c) { |x| x << 10 if x }
p c

# a Hash the block stores another key into
def yh(h) = yield(h)
h = { a: 1 }
yh(h) { |t| t[:k] = "x" }
p h

class P1; def run(a) = yield(a); end
class S1 < P1; def run(a, &b) = super(a, &b); end
class S4 < P1; def run(a, &b) = super; end
a = [+"s"]; S1.new.run(a) { |x| x << 11 }; p a
a = [+"s"]; S4.new.run(a) { |x| x << 12 }; p a

# a keyword a `**` operand or a computed key supplies
def ykw(**h) = yield(**h)
b = [+"s"]; o = { k: b }; ykw(**o) { |k: nil| k << 13 }; p b
pk = proc { |x, k: nil| k << 14 }
b = [+"s"]; o = { k: b }; pk.call(1, **o, **{}); p b
c = [+"n"]; nm = :k; pk.call(1, nm => c); p c
pz = proc { |x, k: 0| x << k }
d = [+"d"]; pz.call(d, **{}); p d

# an Array a main-object ivar or a Struct member holds, also where the
# store's call never runs, and through a method's parameter
def m(x) = x << 15
S = Struct.new(:v)
@a = [+"s"]; y1(@a) { |x| x << 1 } if ARGV.size > 5; p @a
@a = [+"s"]; y1(@a) { |x| x << 16 }; p @a
@a = [+"s"]; m(@a); p @a
s = S.new([+"s"]); y1(s.v) { |x| x << 1 } if ARGV.size > 5; p s.v
s = S.new([+"s"]); y1(s.v) { |x| x << 17 }; p s.v
s = S.new([+"s"]); m(s.v); p s

# an Array an attr_accessor holds, set from outside the class: also where
# the store's call never runs, and through a method's value
class K9; attr_accessor :v; end
def kv(o) = o.v
k = K9.new; k.v = [+"s"]; y1(k.v) { |x| x << 1 } if ARGV.size > 5; p k.v
k = K9.new; k.v = [+"s"]; y1(k.v) { |x| x << 18 }; p k.v
k = K9.new; k.v = [+"s"]; m(kv(k)); p k.v

# beside an instance_variable_set on another object, the attribute's own
# op-writes, and another class's attribute of the same name
class L9; attr_accessor :v; end
k = K9.new; k.v = [+"s"]; [0].instance_variable_set(:@v, 1); k.v += [+"t"]; y1(k.v) { |x| x << 19 }; p k.v
k = K9.new; k.v = [+"s"]; k.v ||= [+"t"]; l = L9.new; l.v ||= [1]; y1(k.v) { |x| x << 20 } if ARGV.size > 5; p [k.v, l.v]

# an attribute handed nil first, a Struct member read by its name or place,
# and members a super of the class's own initialize sets
class K10; attr_accessor :v; end
k10 = K10.new; k10.v = nil; k10.v = [1] if ARGV.size == 0; y1(k10.v) { |x| x << +"z" } if k10.v; p k10.v
S10 = Struct.new(:v)
s10 = S10.new([+"s"]); y1(s10[:v]) { |x| x << 21 }; y1(s10["v"]) { |x| x << 22 }; y1(s10[-1]) { |x| x << 23 }; p s10.v
S11 = Struct.new(:v) do
  def initialize(x) = super([x])
end
s11 = S11.new(+"s"); y1(s11.v) { |x| x << 24 }; p s11.v
D11 = Data.define(:v) do
  def initialize(v:) = super(v: [v])
end
d11 = D11.new(+"s"); y1(d11.v) { |x| x << 25 }; p d11.v

# an attribute beside a send that hands no value and one that names its
# writer or ivar by a computed name, and an attribute read on a receiver
# of more than one class
class K12; attr_accessor :v; end
k12 = K12.new; k12.v = [+"s"]; mn = :v; k12.send(mn); y1(k12.v) { |x| x << 26 }; p k12.v
j12 = K12.new; j12.v = [+"s"]; wn = :v=; j12.send(wn, [+"t"]); y1(j12.v) { |x| x << 27 }; p j12.v
i12 = K12.new; i12.v = [+"s"]; ivn = :@v; i12.instance_variable_set(ivn, [+"u"]); y1(i12.v) { |x| x << 28 }; p i12.v
class Q12 < K12; end
class L12; attr_accessor :v; end
q12 = Q12.new; q12.v = [+"q"]; l12 = L12.new; l12.v = [+"l"]
o12 = ARGV.size > 5 ? k12 : q12; y1(o12.v) { |x| x << 29 }; p o12.v
o12 = ARGV.size > 5 ? q12 : l12; y1(o12.v) { |x| x << 30 }; p o12.v
# and on a receiver that may also hold a class without the reader
class M12; end
n12 = L12.new; n12.v = [+"n"]; o12 = ARGV.size > 5 ? M12.new : n12; y1(o12.v) { |x| x << 31 }; p n12.v
S12 = Struct.new(:w)
s12 = S12.new([+"w"]); p s12.w
T12 = Struct.new(:u, :w)
s12 = T12.new([1], [+"t"]); y1(s12.u) { |x| x << 2.5 }; p s12.u

# an attribute handed another attribute's read, a read on nine classes,
# and computed-name sends of two values and of a keyword
class W13; attr_accessor :w; end
w13 = W13.new; w13.w = [+"x"]; k13 = K12.new; k13.v = w13.w; y1(k13.v) { |x| x << 32 }; p w13.w
C13 = (1..9).map { Class.new { attr_accessor :v } }
c13 = C13.map { |k| o = k.new; o.v = [+"c"]; o }
o13 = c13[ARGV.size]; y1(o13.v) { |x| x << 33 }; p c13[0].v, c13[8].v
class K14; attr_accessor :v; def two(a, b) = a; def kw(a:) = a; end
k14 = K14.new; k14.v = [+"s"]; mn = :two; p k14.send(mn, 1, 2); mn = :kw; p k14.send(mn, a: 1)
y1(k14.v) { |x| x << 34 }; p k14.v

# a reader that is an alias of a method, beside another class's attribute
class L15; def initialize = (@v = [+"l"]); def w = @v; alias v w; end
o15 = [L15.new, K12.new][ARGV.size]; y1(o15.v) { |x| x << 35 }; p o15.v

# a new Array a builtin answers, handed to an attribute's writer and to a
# Struct's constructor
class K16; attr_accessor :v; end
S16 = Struct.new(:w)
k16 = K16.new; k16.v = "ab".bytes; y1(k16.v) { |x| x << +"q" }; p k16.v
s16 = S16.new("cd".bytes); y1(s16.w) { |x| x << +"r" }; p s16.w
# and to a super of a Struct's own initialize
S17 = Struct.new(:v) do
  def initialize(x) = super(x.bytes)
end
s17 = S17.new(+"ab"); y1(s17.v) { |x| x << +"s" }; p s17.v
# and a new Array of to_a or Array.new to an attribute's writer
class K18; attr_accessor :v, :w; end
k18 = K18.new; k18.v = (1..3).to_a; k18.w = Array.new(2, 0)
y1(k18.v) { |x| x << +"t" }; y1(k18.w) { |x| x << +"u" }; p k18.v, k18.w

# a write used as a value (another attribute's writer, a main-object ivar,
# a writer sent by its name), an Array's own to_a, and its entries
class K19; attr_accessor :v; end
class Q19; attr_accessor :v; end
k19 = K19.new; q19 = Q19.new
k19.v = q19.v = [1, 2]; y1(k19.v) { |x| x << +"v" }; p q19.v, k19.v.equal?(q19.v)
@w19 = nil; k19.v = @w19 = [3]; y1(@w19) { |x| x << +"w" }; p k19.v, k19.v.equal?(@w19)
k19.v = q19.send(:v=, [4]); y1(k19.v) { |x| x << +"x" }; p q19.v
a19 = [5]; k19.v = a19.to_a; y1(k19.v) { |x| x << +"y" }; p a19
k19.v = a19.entries; y1(k19.v) { |x| x << +"z" }; p a19, k19.v

# an ivar's own writes the walk follows: a writer's call, a method's value,
# a branch, another attribute's read, a multiple assignment; two attributes
# handed each other; an instance_variable_set as a value, also on a class
# with no writer; and an Array subclass's to_a
class K20; attr_accessor :v, :w, :x, :y, :z; end
class Q20; attr_accessor :v; end
class R20; attr_reader :v; end
class L20 < Array; end
k20 = K20.new; q20 = Q20.new; r20 = R20.new
def mk20 = [6]
@a20 = k20.v = [1]; y1(@a20) { |x| x << +"a" }; p k20.v, k20.v.equal?(@a20)
@b20 = mk20; y1(@b20) { |x| x << +"b" }; p @b20
@c20 = ARGV.size > 9 ? [7] : [8]; y1(@c20) { |x| x << +"c" }; p @c20
k20.w = [2]; @d20 = k20.w; y1(@d20) { |x| x << +"d" }; p k20.w
t20 = [3]; @e20, @f20 = t20, 0; y1(@e20) { |x| x << +"e" }; p t20
q20.v = [4]; k20.x = q20.v; q20.v = k20.x; y1(q20.v) { |x| x << +"f" }; p k20.x
k20.y = q20.instance_variable_set(:@v, [5]); y1(k20.y) { |x| x << +"g" }; p q20.v
u20 = [9]; r20.instance_variable_set(:@v, u20); y1(r20.v) { |x| x << +"h" }; p u20
l20 = L20.new([1]); k20.z = l20.to_a; y1(k20.z) { |x| x << +"i" }; p k20.z, l20

# an ivar's own op-write, `&&=`, a builtin's new Array through a multiple
# assignment and a target beside a splat, each followed as an attribute's
# write is: in a class with a reader and on the main object
class K21
  attr_reader :v, :w
  def initialize = (@v = [1]; @w = [5])
  def grow = (@v += [2]; @w &&= [6, 7])
end
k21 = K21.new; k21.grow
y1(k21.v) { |x| x << +"a" }; y1(k21.w) { |x| x << +"b" }; p k21.v, k21.w
@a21 = [3]; @a21 += [4]; y1(@a21) { |x| x << +"c" }; p @a21
@b21 = [8]; @b21 &&= [9]; y1(@b21) { |x| x << +"d" }; p @b21
@c21 = nil; @c21, @d21 = "ab".bytes, 2; y1(@c21) { |x| x << +"e" }; p @c21, @d21
@e21, *@f21 = [1, 2], 3; y1(@e21) { |x| x << +"f" }; p @e21, @f21

# a multiple assignment's target that takes nil, or its element of what a
# method answers
def two22 = [[1, 2], 3]
def tab22 = [[4], [5]]
@a22 = [5]; @b22, @a22 = 0; y1(@a22) { |x| x << +"a" } if @a22; p @b22, @a22
@c22 = [6]; @d22, *@e22, @c22 = 0; y1(@c22) { |x| x << +"b" } if @c22; p @d22, @e22, @c22
@f22, @g22 = two22; y1(@f22) { |x| x << +"c" }; p @f22, @g22
@h22, @i22 = tab22; y1(@h22) { |x| x << +"d" }; p @h22, @i22
w22, u22 = two22; y1(w22) { |x| x << +"e" }; p w22, u22

# a global, a class variable or a constant reached through a method's
# value, an op-write, a multiple assignment, a write used as a value or
# another of them, each followed as an ivar's writes are
def g23 = $a23
def mk23 = [1, 2]
def h23 = ($c23 ||= [3])
def w23 = ($d23 = [4])
$a23 = [1]; $a23 += [2]; y1(g23) { |x| x << +"a" }; p $a23
$b23 = mk23; y1($b23) { |x| x << +"b" }; p $b23
y1(h23) { |x| x << +"c" }; p $c23
y1(w23) { |x| x << +"d" }; p $d23
$e23 = [5]; $f23 = $e23; $e23 = $f23; y1($f23) { |x| x << +"e" }; p $e23.equal?($f23), $e23
$g23, $h23 = [6], 7; y1($g23) { |x| x << +"f" }; p $g23, $h23
class K23
  @@v = [1]
  @@v += [2]
  def self.v = @@v
end
y1(K23.v) { |x| x << +"g" }; p K23.v
G23 = mk23
def c23 = G23
y1(c23) { |x| x << +"h" }; p G23
