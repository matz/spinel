# A class Struct.new or Data.define makes, with or without a block, is a
# class of the program from its write on: its superclass is Struct or Data,
# a `def C.m`, `class << C`, a header reopen or a class_eval gives it
# methods, an alias of the constant names the same class, and a subclass
# inherits its class methods. A class method called before its def raises
# NoMethodError, as in CRuby.

# a blockless Struct in a module, a def on its path, a subclass inheriting it
module M
  S = Struct.new(:a)
end
def (M::S).k = :k
class T < M::S; end
p M::S.k, T.k, T.new(1).a

# class << C on a blockless Struct, and a header reopen's instance method
G = Struct.new(:a)
class << G
  def make = new(7)
end
class G; def twice = a * 2; end
p G.make.twice

# an alias of a block Struct keeps the first name; a reopen through it
N = Struct.new(:a) { def x = a }
N2 = N
class N2; def y = x + 1; end
p N2.name, N.new(1).y, N2 == N

# poly dispatch over a blockless Struct and Data given methods by class_eval
PA = Struct.new(:v)
PB = Data.define(:v)
PA.class_eval do
  def show = "A#{v}"
end
PB.class_eval do
  def show = "B#{v}"
end
[PA.new(1), PB.new(v: 2)].each { |o| puts o.show }

# an alias of a blockless Struct, reopened through the alias
Foo = Struct.new(:a)
X = Foo
p X.name, X == Foo, X.new(1).a
class X
  def hi = "hi"
end
p Foo.new(1).hi

# class_eval with a block on a blockless Struct
Ce = Struct.new(:a)
Ce.class_eval do
  def m = a + 1
end
p Ce.new(1).m

# class << C on a blockless Struct
Sc = Struct.new(:a)
class << Sc
  def mk = new(9)
end
p Sc.mk.a

# singleton defs, redefined, called directly and from a method, inherited
Rd = Struct.new(:a)
def Rd.k = 1
def go = Rd.k
p Rd.k, go
def Rd.k = 2
p Rd.k, go
class RdSub < Rd
end
p RdSub.k

# a class method called before its def raises, after it answers
Early = Struct.new(:a)
begin
  Early.make
rescue NoMethodError => e
  p e.message
end
def Early.make = new(1)
p Early.make.a, Early.respond_to?(:make)

# an alias of a scoped blockless Struct reopens the original
module A
  B = Struct.new(:x)
end
Ab = A::B
class Ab
  def hi = x + 1
end
p A::B.new(1).hi, Ab.name, Ab == A::B

# a blockless Struct in a class body, given methods by class_eval on its path
class Outer
  Inner = Struct.new(:a)
end
Outer::Inner.class_eval do
  def twice = a * 2
end
p Outer::Inner.new(4).twice, Outer::Inner.new(4).respond_to?(:twice)

# the superclass of a blockless Struct and Data
p Foo.superclass, PB.superclass, Foo.new(1).is_a?(Struct), PB.new(v: 1).is_a?(Data)

# a block Struct and Data inspected inside their block, before the constant
# names them, then reopened by name
Bs = Struct.new(:a) do
  p new(1)
end
class Bs
  def b = a + 1
end
Bd = Data.define(:x) do
  p new(x: 1)
end
class Bd
  def y = x * 2
end
p Bs.new(2), Bs.new(2).b, Bd.new(x: 3), Bd.new(x: 3).y
