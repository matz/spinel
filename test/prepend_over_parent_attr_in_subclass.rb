# A module the parent prepends after a subclass exists stands in front of
# the parent's attr reader for the subclass too.
module M; def v = 50; end
class P5; attr_reader :v; def initialize; @v = 1; end; end
class D5 < P5; end
class P5; prepend M; end
p P5.new.v
p D5.new.v
[P5.new, D5.new].each { |o| p o.v }

module N3; def v3 = 70; end
class Q3; attr_accessor :v3; end
class R3 < Q3; end
class Q3; prepend N3; end
r = R3.new; r.v3 = 2; p r.v3

# a def in the parent after its attr_accessor answers the subclass too
class Q4; attr_accessor :w; end
class R4 < Q4; end
class Q4; def w = 80; end
s = R4.new; s.w = 3; p s.w

# a writer the module defines
module W; def x=(v); @x = v * 10; end; end
class Q5; attr_accessor :x; end
class R5 < Q5; end
class Q5; prepend W; end
t = R5.new; t.x = 4; p t.x
