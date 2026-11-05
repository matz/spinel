class V
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); (n - o.n) * (2**70); end
  def succ; V.new(n + 1); end
  def to_s; "V#{n}"; end
end
class W
  attr_reader :s
  def initialize(s); @s = s; end
  def <=>(o); o.is_a?(W) ? s <=> o.s : (o.is_a?(Regexp) ? 0 : nil); end
end
r = V.new(1)..V.new(3)
puts r.map(&:to_s).join(",")
puts V.new(2) > V.new(1)
puts r.include?(V.new(2))
wr = W.new("a")..W.new("c")
case /x/
when wr then puts "regexp in"
else puts "regexp out"
end
