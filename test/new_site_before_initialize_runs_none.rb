# A `new` or `raise` that runs before the program defines its class's
# initialize runs none of the program's: the builtin one, which sets no
# ivar and takes the members as given. A later site runs the one defined.

class K
  attr_reader :a, :f
end
k1 = K.new
r = [1, 2].map { |i| K.new.a }
class K
  def initialize
    @a = 1
    @f = 1.5
  end
end
k2 = K.new
p [k1.a, k1.f, k2.a, k2.f], r

class Y
  attr_reader :v
end
y = Y.new { 1 }
class Y
  def initialize = @v = yield
end
p y.v, Y.new { 2 }.v

S = Struct.new(:a, :b)
s1 = S.new(1, 2)
class S
  def initialize(a, b) = super(a * 10, b * 10)
end
p s1.to_a, S.new(1, 2).to_a

B = Struct.new(:a)
class T < B; end
t1 = T.new(1)
class T
  def initialize(a) = super(a * 7)
end
p t1.a, T.new(1).a

D = Data.define(:a, :b)
d1 = D.new(a: 1, b: 2)
class D
  def initialize(a:, b: 5) = super(a: a, b: b * 2)
end
p d1, D.new(a: 1, b: 2), D.new(a: 7)

class E < StandardError; end
e1 = E.new
begin
  raise E, "first"
rescue E => e
  p e.message
end
class E
  def initialize(m = "dflt") = super("[" + m + "]")
end
begin
  raise E, "second"
rescue E => e
  p e.message
end
p e1.message, E.new.message

# A `new` on a Class value builds each class as its site would: before a
# class's initialize exists, none of the program's runs.
class CK; attr_reader :a; end
class CJ; attr_reader :a; def initialize(v = 0) = (@a = v + (block_given? ? yield : 0)); end
CS = Struct.new(:a, :b)
class CE < StandardError; end
ck, cs, ce = [CK, CJ].first, [CS, CJ].first, [CE, CJ].first
c1, c2, c3 = ck.new { 4 }, cs.new(1, 2), ce.new("m")
class CK
  def initialize(v = 0) = (@a = v + yield + 1)
end
class CS
  def initialize(a, b) = super(a * 10, b * 10)
end
class CE
  def initialize(m) = super("[" + m + "]")
end
p c1.a, ck.new(1) { 4 }.a, c2.to_a, cs.new(1, 2).to_a, c3.message, ce.new("m").message

# Keywords spread from an empty Hash reach a class with no initialize yet
# as no argument, as `*[]` does; any key is one too many, and an exception
# class takes the Hash as its message. A block is no argument, and each
# positional or literal keyword one too many.
class CL; attr_reader :a; end
class CF < StandardError; end
cl, cf = [CL, CJ].first, [CF, CJ].first
h, h2 = {}, { z: 1 }
c4 = cl.new(**h)
r = [begin; cl.new(**h2); rescue ArgumentError => e; e.message; end,
     begin; cl.new(1) { 4 }; rescue ArgumentError, NoMethodError => e; e.class; end,
     begin; cl.new(1, k: 2); rescue ArgumentError, NoMethodError => e; e.message; end,
     cf.new(**h).message, cf.new(**h2).message]
class CL
  def initialize(v = 0) = (@a = v + yield)
end
class CF
  def initialize(m = "x") = super("<" + m + ">")
end
p c4.class, c4.a, r, cl.new(1) { 4 }.a, cf.new.message
