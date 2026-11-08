# An implicit to_ary conversion calls a private or protected to_ary, as
# CRuby's rb_check_array_type does: flat_map splices the result, and an
# Array operator takes it as its operand. A plain v.to_ary call still
# raises NoMethodError.
class Priv; private def to_ary = [:p, :q]; def inspect = "#<Priv>"; end
class Prot; protected def to_ary = [:r]; def inspect = "#<Prot>"; end
class PNil; private def to_ary = nil; def inspect = "#<PNil>"; end
class PBad; private def to_ary = 5; def inspect = "#<PBad>"; end
v = Priv.new
w = Prot.new
p [1] + v, [1, :p] - v, [1] | v, [:p, 2] & v, [:p, :q] <=> v
p [1].union(v), [1, :p].difference(v), [:q].intersection(v)
p [:z].replace(v)
p [1] + w, [1].flat_map { w }
p [1].flat_map { PNil.new }
begin; [1].flat_map { PBad.new }; rescue TypeError => e; puts e.message; end
x = [v, 1].first
p [1] + x
p [1].flat_map { x }
y = [w, 1].first
p [1].flat_map { y }
def each_obj = [Priv.new, PNil.new, 3, [4]].each { |o| yield o }
r = []
each_obj { |o| r.concat([1].flat_map { o }) }
p r
p [1, 2].each_slice(1).flat_map { v }
p v.respond_to?(:to_ary)
begin; v.to_ary; rescue NoMethodError => e; puts e.message; end

# a program respond_to? with one parameter, or two required ones
class R1
  def respond_to?(n) = n == :to_ary || super
  def to_ary = [:r1]
end
p [1].flat_map { R1.new }
class R2; def respond_to?(n) = super(n); def inspect = "#<R2>"; end
p [1].flat_map { R2.new }
class R3; def respond_to?(n, p) = super; private def to_ary = [:r3]; end
p [1].flat_map { R3.new }
