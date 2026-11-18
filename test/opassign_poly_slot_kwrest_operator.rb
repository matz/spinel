# `x OP= rhs` on a boxed local that may hold a K whose `+` takes only a
# `**` parameter: CRuby raises ArgumentError (the right-hand side is a
# positional argument). The op-write leaves the `**` parameter to the
# call's own binding rather than typing it as the right-hand side, which
# made its C read a Hash as an Integer.
class K
  attr_reader :x
  def initialize(x) = @x = x
  def +(**kw) = K.new(@x + kw.fetch(:k, 1))
end
p (K.new(1).+(k: 5)).x
s = [K.new(10), 3][ARGV.size]
begin
  s += 2
  p s.x
rescue ArgumentError => e
  p e.class
end
