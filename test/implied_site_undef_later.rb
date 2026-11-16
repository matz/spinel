# A method undefined after a site is still the method at that site: the
# implied calls (a Comparable operator's <=>, a conversion, a pattern's
# deconstruct, a when's ===, a display) are answered at their site's point.
class K
  include Comparable
  def <=>(o); 0; end
  def to_int; 2; end
  def to_str; "k"; end
  def to_s; "K-text"; end
  def inspect; "K-insp"; end
  def deconstruct; [1, 2]; end
  def ===(o); true; end
end
k = K.new
p(k < K.new)
p [10, 20, 30][k]
p Integer(k)
p "x" + k
puts "#{k}"
puts k
p k
case k
in [x, y] then p [x, y]
end
case 1
when k then puts "matched"
end
class K
  undef_method :<=>, :to_int, :to_str, :to_s, :inspect, :deconstruct, :===
end
puts "end"
