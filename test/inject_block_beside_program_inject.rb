# A block handed to inject on a value that is an Array or a program class
# with an inject of its own: the builtin fold typed the block's nodes for
# its accumulator (the seed's Array) and left them so, and the same block
# emitted again as the proc the class's inject takes read its boxed `a` as
# that Array; the C did not build (#10931, fugit's compact_month_days
# beside a Relation#inject).
class Rel
  def initialize(xs) = @xs = xs
  def to_a = @xs
  def inject(initial)
    acc = initial
    to_a.each { |x| acc = yield(acc, x) }
    acc
  end
end
months = ARGV.size > 5 ? Rel.new([1, 2]) : [3, 4]
days = [10, 20]
ms, ds = months.inject([[], []]) { |a, m| days.each { |d| a[0] << m; a[1] << d }; a }
p ms, ds
r = (ARGV.size > 5 ? [5] : Rel.new([6, 7])).inject(0) { |s, x| s + x }
p r
q = (ARGV.size > 5 ? Rel.new([1]) : [1, 2]).inject([]) { |a, m| a << m; a }
p q
