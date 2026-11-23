# locals the desugars make beside same-named locals of other scopes: a repeated `_a`, a reassigned block parameter, a Class.new parameter, builtin iterator blocks, table-loop copies and a hook splice
_a = :outer
p [[1, 2]].map { |_a, _a| _a }, _a
pr = proc { |v| v = v.to_s + "!"; v }
v = :v_outer
p pr.call(1), v
K = Class.new do |k|
  k = "reassigned"
  define_method(:kk) { k }
end
p K.new.kk
x = "user x"
p [3, 1, 2].min_by { |x| -x }, [1, 2, 2].count { |x| x == 2 }, [5, 6].find_index { |x| x > 5 }, x
class T
  [:a, :b].each { |nm| define_method(nm) { |q = 0| [nm, q] } }
end
p T.new.a, T.new.b(:z)
module H
  def self.included(b); y = b.name; b.define_method(:hy) { y }; end
end
y = :top_y
class U; include H; end
p U.new.hy, y
