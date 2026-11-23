# A class body in a value that defines `initialize` is held: recorded as a body the record does not place, as it was
# before bodies in a value were placed. So is any body in a value with a version of a name a held body defines, by
# whatever form makes it (an attr, a word around an attr, a second attr name, a definer call), and any body such a
# body reaches in turn, however long the chain. Each call answers the table's last def, as CRuby does here.
class K1; def v = :top; end
z = [(class K1; attr_reader :v; end; 0)]
z = [(class K1; def initialize; @v = :ivar; end; def v = :def; end; 0)]
p K1.new.v

class K2; def v = :top; end
z = [(class K2; public attr_reader :v; end; 0), (class K2; def initialize; @v = :ivar; end; def v = :def; end; 0)]
p K2.new.v

class K3; def v = :top; end
z = [(class K3; attr_accessor :u, :v; end; 0)]
z = [(class K3; private def initialize; @v = :ivar; end; def v = :def; end; 0)]
o = K3.new
p o.v, o.send(:v), o.method(:v).call, [o].map(&:v), [o, nil][0]&.v

class K4; def v = :top; end
module M4; attr :v; end
z = [(class K4; include M4; def initialize; @v = :ivar; end; def v = :def; end; 0)]
p K4.new.v

# a chain: the first body is held by its initialize, each later one by a name the one before it defines
class K5; def a0 = 0; end
z = [(class K5; def initialize; end; def a1 = 1; end; 0)]
z = [(class K5; def a1 = 2; attr_reader :a2; end; 0)]
z = [(class K5; def a2 = 3; define_method(:a3) { 4 }; end; 0)]
z = [(class K5; def a3 = 5; alias_method :a4, :a3; end; 0)]
z = [(class K5; def a4 = 6; end; 0)]
k = K5.new
p [k.a1, k.a2, k.a3, k.a4]
