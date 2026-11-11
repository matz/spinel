# A method's named &blk, read beside the forward, forwarded to an iterator
# on a boxed receiver: called without a block, the iterator answers its
# Enumerator, as CRuby's does
def m(x, &blk)
  p blk.nil?
  x.map(&blk)
end
def pick(i) = [[1, 2], { a: 1, b: 2 }][i]

p m(pick(0)) { |v| v * 3 }
p m(pick(1)) { |k, v| "#{k}=#{v}" }
p m(pick(1)) { |pair| pair }
e = m(pick(0))
p e.class
p e.map { |v| v + 1 }
p m(pick(1)).class

# a lambda handed on as the block
p m(pick(0), &->(v) { v - 1 })

# the block also called directly (a Hash's select yields two values)
def sel(x, &blk)
  first = blk ? blk.call(0) : :none
  [first, x.select(&blk)]
end
p sel(pick(0)) { |v| v.even? }
p sel(pick(1)) { |k, v| v.is_a?(Integer) && v > 1 }

# other iterators
def many(x, &blk)
  p blk.nil?
  [x.flat_map(&blk), x.sort_by(&blk), x.each(&blk)].map(&:class)
end
p many(pick(0))
p many(pick(1))
p many(pick(0)) { |v| [-v] }
