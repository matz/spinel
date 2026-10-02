# A yielding method of an Array, Hash or Numeric reopening, called on a
# receiver of that kind with a block or without one -- activesupport's
# Array#in_groups_of and Hash#deep_merge. Such a call is not spliced in;
# the reopen fallback called a function the method never had (only the
# Object reopening reached its proc form), and the C did not build. It now
# calls the proc form, with the call's block or none.
class Array
  def each_pair_sum(&block)
    sums = []
    each_with_index { |x, i| sums << x + i }
    block_given? ? sums.each(&block) : sums
  end
end

class Hash
  def each_key_twice
    out = []
    keys.each { |k| out << (block_given? ? yield(k) : k) }
    out + out
  end
end

class Numeric
  def times_yield
    block_given? ? yield(self * 2) : self * 3
  end
end

p [1, 2, 3].each_pair_sum
[1, 2, 3].each_pair_sum { |s| p s }
p({ a: 1, b: 2 }.each_key_twice)
p({ a: 1 }.each_key_twice { |k| k.to_s })
p 4.times_yield
p(4.times_yield { |v| v + 1 })
p 1.5.times_yield
