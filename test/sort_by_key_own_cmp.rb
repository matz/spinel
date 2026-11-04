# spinel: gc-stress
# A class's own <=> that nothing names, only the keys of a sort_by reach, is
# kept: keys of such a class are ordered through a literal block and through
# a forwarded proc, on a typed and on a boxed Array.
class Ver
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = o.n <=> n   # descending
end

f = proc { |x| Ver.new(x) }
p [1, 3, 2].sort_by { |x| Ver.new(x) }
p [1, 3, 2].sort_by(&f)
v = ARGV.size > 5 ? 1 : [1, 3, 2]
p v.sort_by(&f)
p v.sort_by { |x| Ver.new(x) }
