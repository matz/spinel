# A boxed value that may be a Class with its own `self.[]` reaches that
# class method with the call's block, by one argument or by two: the
# class-method dispatch takes no literal block, and the per-class dispatch
# does.
class Bag
  def self.[](a, b = 0, &blk) = blk ? blk.call(a, b) : a
end

v = [Bag, {"a" => 1}].first
p(v["a"] { |k, _| k * 2 })
p(v[1, 2] { |a, b| a + b })
w = [{"a" => 1}, [1, 2, 3]]
p(w.first["a"] { |k| k })
p(w.last[1, 2] { |a| a })
