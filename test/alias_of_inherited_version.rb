# An alias of an inherited method is a method of the aliasing class made of
# the version the alias's point has: an inherited method that calls it
# through self reaches it by dispatch, whatever an ancestor or a descendant
# does with the original name afterwards.
class RS
  def nxt = :array
  def nxt_hash = :hash
  def each = nxt
end
class HRS < RS
  alias_method :nxt, :nxt_hash
  p new.each
  def nxt_hash = :redef
end
class HRS2 < HRS
  def nxt_hash = :overridden
end
p RS.new.each, HRS.new.each, HRS2.new.each, HRS.new.nxt_hash

# reflection: the alias's class owns it, the original's name is its
# original_name, and it takes the original's arity and parameters
class A
  def m(x, y = 2, *r, k: 1, &b) = [x, y, r, k]
end
class B < A
  alias_method :n, :m
end
um = B.instance_method(:n)
p um.owner, um.name, um.original_name, um.arity, um.parameters
p B.new.method(:n).owner, B.new.n(1, 3, 4, k: 5)
p B.instance_methods(false), B.method_defined?(:n), A.method_defined?(:n)

# a grandparent's method, String names, a chain across classes
class G
  def a = :a
  def go = [b, c]
end
class P < G; end
class B1 < P
  alias_method "b", "a"
end
class C1 < B1
  alias c b
  def a = :c_a
end
p C1.new.b, C1.new.c, C1.new.go, C1.new.a
p C1.instance_method(:c).original_name, B1.instance_method(:b).original_name
p C1.instance_method(:c).owner, B1.instance_method(:b).owner

# the ancestor redefines the original after the alias
class Q
  def m = :first
end
class R < Q
  alias n m
end
class Q
  def m = :second
end
class R
  def other = n
end
p R.new.n, R.new.other, R.new.m

# two subclasses alias one name to two methods
class T
  def x = :tx
  def y = :ty
  def run = z
end
class T1 < T
  alias z x
end
class T2 < T
  alias z y
end
p T1.new.run, T2.new.run

# a block, keywords and a default argument go through
class U
  def sum(a, b = 10, *rest, scale: 1)
    yield (a + b + rest.sum) * scale
  end
end
class V < U
  alias_method :add, :sum
end
V.new.add(1) { |v| p v }
V.new.add(1, 2, 3, scale: 2) { |v| p v }

# a private method stays private through the alias
class W
  private
  def m = 1
end
class X < W
  alias n m
  def go = n
end
p X.new.go, X.new.respond_to?(:n), X.private_method_defined?(:n)

# an included module's method, self-dispatched from a class that includes it
module M
  def m = :m
end
class IP
  include M
  def call_n = n
end
class IC < IP
  alias n m
end
p IC.new.n, IC.new.call_n

# a body that calls super keeps its owner's super, through a class, a
# prepended module and an included module
class SBase
  def m(x, y = 1) = [:base, x, y]
end
class SA < SBase
  def m(x, y = 2) = [:a, super, super(x + 10, y)]
  def call_n = n(1)
end
class SB < SA
  alias n m
end
class SC < SB
  def m(x, y = 3) = [:c, super]
end
p SB.new.n(5), SB.new.call_n, SC.new.call_n, SC.new.n(7, 8), SC.new.m(1)
module Pre
  def m = [:pre, super]
end
class PP
  prepend Pre
  def m = :p
  def call_n = n
end
class PC < PP
  alias n m
end
p PC.new.n, PC.new.call_n, PC.new.m
module Loud
  def m = [:loud, super]
end
class LBase
  def m = :base
end
class LP < LBase
  include Loud
  def call_n = n
end
class LC < LP
  alias n m
end
p LC.new.n, LC.new.call_n, LC.new.m

# an alias in a module of the method of a module it includes, which an
# includer then has, with a super in it too
module MN
  def m = :n_m
end
module MM
  include MN
  alias k m
end
class MC
  include MM
end
p MC.new.k
module SN
  def m = [:n] + super
end
module SM
  include SN
  alias k m
end
class SBase2
  def m = [:base]
end
class SC2 < SBase2
  include SM
end
p SC2.new.k, SC2.new.m

# an alias of an initialize ends in the initialize it reaches
class IBase
  def initialize(v) = @v = v
  attr_reader :v
end
class IA < IBase
  def initialize(v)
    super(v * 2)
  end
end
class IB < IA
  alias reinit initialize
  public :reinit
end
ib = IB.new(1)
ib.reinit(5)
p ib.v

# a super into a builtin's method, a Kernel method and a Struct writer
class LA < Array
  def push(x) = super(x * 2)
end
class LB < LA
  alias add push
end
lb = LB.new
lb.add(3)
p lb
class KA
  def inspect = "A<" + super[0, 3] + ">"
  def ==(o) = super
end
class KB < KA
  alias show inspect
  alias same ==
end
kb = KB.new
p kb.show, kb.same(kb), kb.same(KB.new)
class WP < Struct.new(:a)
  def a=(v)
    super(v * 10)
  end
end
class WQ < WP
  alias_method :b=, :a=
end
wq = WQ.new(1)
wq.b = 2
p wq.a

# the Method of an alias to a proc, of a method that calls super with a block
class YBase
  def m
    yield 1
  end
end
class YA < YBase
  def m(&b)
    super(&b)
  end
end
class YB < YA
  alias n m
end
p YB.new.method(:n).to_proc.call { |v| v * 3 }

# an alias of the writer of a Struct member: in a Struct.new block, in a
# class that inherits the Struct, over its own writer, and never called
SMB = Struct.new(:a) do
  alias_method :put, :a=
  alias_method :unused=, :a=
end
smb = SMB.new(1)
smb.put(4)
p smb.a
class SI < Struct.new(:a)
  alias_method :b=, :a=
end
si = SI.new(1)
si.b = 2
p si.a
class SO < Struct.new(:a)
  def a=(v)
    super(v * 10)
  end
  alias_method :b=, :a=
end
so = SO.new(1)
so.b = 2
p so.a

# ...and a later def of the writer, in a reopening, does not reach the alias
SQ = Struct.new(:a) do
  alias_method :b=, :a=
end
class SQ
  def a=(v)
    super(v * 10)
  end
end
sq = SQ.new(1)
sq.b = 3
p sq.a
class SR < Struct.new(:a)
  alias_method :b=, :a=
end
class SR
  def a=(v)
    @seen = v
    super(v * 10)
  end
end
sr = SR.new(1)
sr.b = 3
p sr.a
sr.a = 5
p sr.a

# an alias of a Struct member's writer two classes below the Struct, with a
# slot of its own beside it, and a defined? that never runs the alias
class GP < Struct.new(:a)
end
class GQ < GP
  attr_accessor :z
  alias_method :b=, :a=
end
gq = GQ.new(1)
gq.z = 5
gq.b = 2
p gq.a, gq.z, gq.to_a, GP.new(3).a
class DB
  def initialize(v) = @v = v
  attr_reader :v
end
class DC < DB
  alias reinit initialize
  public :reinit
end
dc = DC.new(1)
p defined?(dc.reinit(2)), dc.v
