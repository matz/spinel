# A program that reopens Enumerable keeps the builtin Enumerable methods
# (max_by, min_by, each_with_object, each_slice, group_by, ...) for its own
# includers and the builtin receivers, beside the methods it adds; one it
# redefines is its own.
module Enumerable
  def tally_by_parity = group_by(&:odd?).transform_values(&:size)
end
class E2; include Enumerable; def each = (yield 3; yield 1; yield 4); end
p E2.new.max_by { |x| x }
p E2.new.min_by { |x| -x }
p E2.new.tally_by_parity
p [3, 1].max_by { |x| x }, [1, 2, 3].each_with_object([]) { |x, a| a << x * 2 }
p (1..4).each_slice(2).to_a
p({a: 1}.min_by { |k, v| v })
module Enumerable; def second = to_a[1]; end
class E4; include Enumerable; def each = (yield 3; yield 1); end
p E4.new.second, E4.new.max_by { |x| x }
