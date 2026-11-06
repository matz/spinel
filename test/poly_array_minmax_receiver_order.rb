# Array#minmax asks the extreme so far (`best <=> v`); #min and #max ask the
# new element (`v <=> best`). CRuby runs the min and max passes in an order
# its C compiler picks, so the calls are compared as a sorted list.

$calls = []

class A
  attr_reader :v
  def initialize(v); @v = v; end
  def <=>(o)
    return nil unless o.is_a?(A)
    $calls << "A #{v}<=>#{o.v}"
    v <=> o.v
  end
end

class C < A
  def <=>(o)
    $calls << "C #{v}<=>#{o.v}"
    -(v <=> o.v)
  end
end

def show(r)
  p r
  p $calls.sort
  $calls = []
end

show [A.new(3), C.new(1)].minmax.map(&:v)
show [C.new(1), A.new(3)].minmax.map(&:v)
show [A.new(3), C.new(1)].max.v
show [A.new(3), C.new(1)].min.v

[[1], [2, 1], [3, 1, 2], [4, 3, 2, 1, 0], [5, 1, 4, 2, 3]].each do |vs|
  show vs.map { |x| A.new(x) }.minmax.map(&:v)
end
p [].minmax

begin
  [A.new(1), 3].minmax
rescue ArgumentError => e
  p e.message
end
begin
  [3, A.new(1)].minmax
rescue ArgumentError => e
  p e.message
end
