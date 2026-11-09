# A def (or alias, or define_method) that a later reopen of a class adds
# over the class's attr_reader or attr_writer answers for its subclasses
# too, also for a subclass declared before the reopen.
class P
  attr_accessor :v, :w
  def initialize = (@v = 1; @w = 2)
end
class D < P; end
class G < D; end
class P
  def v = 40
  def w=(x); @w = x * 10; end
end
p D.new.v, G.new.v, P.new.v
d = D.new; d.w = 3; p d.w
g = G.new; g.w = 4; p g.w
# reader again after the def: the reader wins
class Q
  attr_reader :a
  def initialize = @a = 1
end
class E < Q; end
class Q
  def a = 50
  attr_reader :a
end
p E.new.a
# alias over the reader
class R
  attr_reader :b
  def initialize = @b = 1
  def bb = 7
end
class F < R; end
class R
  alias b bb
end
p F.new.b
# define_method over the reader
class S
  attr_reader :c
  def initialize = @c = 1
end
class H < S; end
class S
  define_method(:c) { 60 }
end
p H.new.c
# grandparent reader, parent def
class T
  attr_reader :e
  def initialize = @e = 1
end
class U < T; end
class V < U; end
class U
  def e = 70
end
p V.new.e, T.new.e
# the subclass's own reader stays
class K
  attr_reader :f
  def initialize = @f = 1
end
class L < K
  attr_reader :f
end
class K
  def f = 80
end
p L.new.f, K.new.f
# boxed receivers
p [D.new, P.new, G.new].map(&:v)
x = [D.new, 1].first
p x.v
# super from subclass
class M < P
  def v = super + 1
end
p M.new.v
# a module the parent includes later does not replace the parent's reader
class PM
  attr_reader :v
  def initialize = @v = 1
end
class DM < PM; end
module Mx
  def v = 90
end
class PM
  include Mx
end
p DM.new.v, PM.new.v
# a def made private after the reader
class P3
  attr_reader :v
  def initialize = @v = 1
end
class D3 < P3; end
class P3
  def v = 40
  private :v
end
p((D3.new.v rescue $!.class))
