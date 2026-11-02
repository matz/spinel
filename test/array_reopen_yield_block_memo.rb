# A block call on an Array or a Hash whose reopened method of the name
# yields, where the builtin surface has no arm of the name: the call runs
# the reopen's method through its proc form, with the block as a proc. The
# call plan binds the call to that method, so the block's parameters take
# what the method yields (a boxed memo, a boxed element), not the builtin
# shape of the name. `m` typed from the builtin each_with_object read the
# boxed memo as a typed Array and crashed (#10719).
class Array
  def each_with_object(m) = (each { |e| yield(e, m) }; m)
  def each_pair_sum(acc) = (i = 0; while i < size; yield(self[i], acc); i += 1; end; acc)
end
class Hash
  def each_with_object(m) = (each { |k, v| yield([k, v], m) }; m)
end
class H
  def initialize = (@arr = [3, 1]; @h = { a: 1, b: 2 })
  def run
    p(@arr.each_with_object([]) { |e, m| m << e })
    p(@arr.each_with_object({}) { |e, m| m[e] = e * 2 })
    p(@arr.each_pair_sum([0]) { |e, acc| acc[0] += e })
    p(@h.each_with_object([]) { |(k, v), m| m << "#{k}=#{v}" })
    total = 0
    @arr.each_with_object(nil) { |e, _| total += e }
    p total
  end
end
H.new.run
