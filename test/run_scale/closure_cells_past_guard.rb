# run-scale: 1000 | 16000 | 1.5
# A method that opens with a guard and holds a block it cannot inline (the
# receiver may be a class with its own #each): a call the guard turns away
# reaches no block, so it makes none of the block's cells, and the bytes stay
# flat when such calls run 16x.
class Bag
  def each
    yield "z"
  end
end
class Route
  def initialize(segments) = @segments = segments
  def match(segs)
    return nil if segs.empty?
    captured = {}
    @segments.each { |seg| captured[seg] = segs[0] }
    captured
  end
end
p Route.new(Bag.new).match(["y"])
r = Route.new(["a", "b"])
none = []
n = 0
ARGV[0].to_i.times { n += 1 if r.match(none).nil? }
p n
