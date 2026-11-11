# A boxed program object has none of Array's own names (at, last, join,
# sample, rotate, shuffle, flatten, values_at) unless its class defines
# them: each raises NoMethodError, as in CRuby. Before, a Struct's members
# or an Enumerable object's #each answered them as if it were an Array.
module HasAt
  def at(i) = "mod#{i}"
end

module ML
  def last(n = nil) = n ? [:ml, n] : :ml
end

class Foo
  def at(i) = "foo#{i}"
end

class En
  include Enumerable
  def each
    yield 1
    yield 2
  end
end

class Rd < En
  include ML
  attr_reader :join
  def initialize
    @join = :own_join
  end
end

class Al < En
  def pick(*a) = [:pick, *a]
  alias sample pick
  alias rotate pick
end

S = Struct.new(:a, :b)
T = Struct.new(:a) do
  def at(i) = "T#{i}"
end
U = Struct.new(:a) { include HasAt }
class V < Struct.new(:a, :b); end
W = Struct.new(:last, :join)

def probe(v) = (v.at(1) rescue $!.class)

vals = [S.new(1, 2), Foo.new, [7, 8], T.new(2), U.new(3), V.new(4, 5), W.new(5, 6),
        En.new, Rd.new, Al.new, "str", nil, :sym, (1..3)]
vals.each do |v|
  p [(v.at(0) rescue $!.class), probe(v), (v.last rescue $!.class), (v.join rescue $!.class),
     (v.rotate rescue $!.class), (v.rotate(1) rescue $!.class), (v.sample(1).size rescue $!.class),
     (v.shuffle.size rescue $!.class), (v.flatten rescue $!.class), (v.values_at(0) rescue $!.class)]
end
