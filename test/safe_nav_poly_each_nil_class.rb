# `v&.each` on a boxed `v` holds the Enumerator in a typed slot, NULL when
# `v` is nil, and `class` there named the slot's type: Enumerator where
# CRuby answers NilClass. Through each, reverse_each and each_entry, a nil,
# a Hash and an Array, and a chain, which keeps Enumerator::Chain.
def pick(i) = [nil, {a: 1}, [3, 1]][i]
[0, 1, 2].each do |i|
  s = pick(i)&.each
  p s.class
  p s.class == NilClass
  t = pick(i)&.reverse_each if i != 1
  p t.class
  u = pick(i)&.each_entry if i != 1
  p u.class
end
e = [1].each.chain([2])
p e.class
