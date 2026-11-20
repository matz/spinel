# minmax on a boxed value that may be a Float Range or a String Range,
# beside a class of the program with its own minmax: a Range answers its
# endpoints, as CRuby's Range#minmax does, rather than a walk (a Float
# Range cannot be walked), and the Float or String Array it answers is
# held as the boxed Array the call answers. A class with an #each but no
# Enumerable has no minmax.

class Span
  def minmax = [:lo, :hi]
end

class Walk
  def each = yield(1)
end

def pick(i) = [(1.5..2.5), ("a".."c"), (1..4), [3, 1, 2], Span.new, Walk.new][i]

0.upto(4) { |i| p pick(i).minmax }
begin
  pick(5).minmax
rescue NoMethodError => e
  p e.class
end

def by_name(i, names)
  names.map { |n| pick(i).public_send(n) }
end

p by_name(0, [:minmax]), by_name(1, [:minmax]), by_name(4, [:minmax])
