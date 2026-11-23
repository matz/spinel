# arr.each_with_index.map { |x, i| } and arr.each.with_index.map { |x, i| }
# answer an Array of the block's values, typed as arr.map.with_index { }'s
# is, made at the receiver's length and filled slot by slot. Every shape
# keeps CRuby's answers: a value that may be nil, a next, a break, a block
# that shrinks or grows the receiver, an offset, a one-parameter block.
def fmap(a) = a.each_with_index.map { |x, i| x + i }
def imap(a) = a.each_with_index.map { |x, i| x * i }
def wmap(a) = a.each.with_index.map { |x, i| x - i }
def wmap1(a) = a.each.with_index(1).map { |x, i| x * i }
def nilmap(a) = a.each_with_index.map { |x, i| i.odd? ? x : nil }
def nextmap(a) = a.each_with_index.map { |x, i| next 0.5 if i == 1; x * 2 }
def brkmap(a) = a.each_with_index.map { |x, i| break i if x > 2.0; x }
def popmap(a) = a.each_with_index.map { |x, i| a.pop if i == 0; x + i }
def pushmap(a) = a.each_with_index.map { |x, i| a << 9 if i == 0; x + i }
def onemap(a) = a.each_with_index.map { |pair| pair }
def strmap(a) = a.each_with_index.map { |s, i| s * i }

f = [1.5, 2.5, 3.5]; n = [3, 4, 5]
p fmap(f), imap(n), wmap(f), wmap1(n), nilmap(f), nextmap(f), brkmap(f), brkmap([1.0])
p popmap([1, 2, 3]), pushmap([1, 2]), onemap(n), strmap(%w[a b c])
p fmap([]), imap([7]), fmap(f).sum, imap(n).sum, fmap(f).class, wmap(f).first

# a valued next widens the Array past the tail's kind (infer_map_block_ty)
def nextw(f)
  _, b = f ? [1, 2.5] : [3, 4]
  [10, 20, 30].each.with_index(1).map { |x, i| next b if i == 2; x + i }
end
p nextw(true), nextw(false)
