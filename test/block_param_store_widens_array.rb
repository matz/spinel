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
