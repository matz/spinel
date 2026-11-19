# A call in a block the record keeps to run later (a Hash default block, an
# Enumerator's, a lazy chain's, a define_method body, a Struct's method, a
# Proc.new body) has a point of its own, but may run from an exit handler: it
# meets the version the handler makes, not only the one live at its point.
class E; def baz = "baz"; end
class F; def baz = "baz"; end
class G; def baz = "baz"; end
class I; def baz = "baz"; end
class J; def baz = "baz"; end
class K; def baz = "baz"; end

h = Hash.new { |_, k| [k, E.new.baz] }
e = Enumerator.new { |y| y << F.new.baz }
lz = (1..3).lazy.map { |v| [v, G.new.baz] }
class Dm; define_method(:run) { I.new.baz }; end
S = Struct.new(:a) { def run = J.new.baz }
pr = Proc.new { K.new.baz }
p([:load, h[1], e.first, lz.first(1), Dm.new.run, S.new(1).run, pr.call])
at_exit { p([:last, h[2], e.first, lz.first(1), Dm.new.run, S.new(1).run, pr.call]) }
at_exit do
  class E; def baz = "new"; end
  class F; def baz = "new"; end
  class G; def baz = "new"; end
  class I; def baz = "new"; end
  class J; def baz = "new"; end
  class K; def baz = "new"; end
  nil
end
