# A call that runs before a later reopen (`class V; def self.new`) defines
# a class method meets the builtin (or no method), not that late def.
# The class is a Struct or Data class made at a write with no block, a
# nested class, or a plain class.

V = Struct.new(:z)
def vn = V.new(1)
p vn
class V; def self.new(*a) = :custom; end
p vn

D = Data.define(:z)
def dn = D.new(z: 1)
p dn
class D; def self.new(*a, **k) = :custom; end
p dn

T = Struct.new(:z)
def tm = T.members
p tm
class T; def self.members = [:late]; end
p tm

M = Struct.new(:z)
p M.respond_to?(:make)
def mk = (M.make rescue :no_method)
p mk
class M; def self.make = :made; end
p mk
p M.respond_to?(:make)

class C; def initialize(x = 0) = @x = x; end
p C.new(1).class
p [1, 2].map { |i| C.new(i).class }
class C; def self.new(*a) = :custom; end
p C.new(1)

class E; def initialize(x) = @x = x; def x = @x; end
p E.new(5).x
class E; def self.new(*a) = super(a.first * 2); end
p E.new(5).x

module N; class C; def hi = :hi; end; end
p N::C.new.hi
class N::C; def self.new(*a) = :late; end
p N::C.new

module N; S = Struct.new(:z); end
def sn = N::S.new(1)
p sn
class N::S; def self.new(*a) = :custom; end
p sn

class G; def hi = :hi; end
def setup_g = G.class_eval { def self.new(*a) = :late }
setup_g
p G.new
