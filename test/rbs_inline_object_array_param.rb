# An inline signature naming an Array of a program class (`Array[Point]`)
# was dropped whole -- "names no type Spinel can pin" -- so its other types
# (the Integer, the Float) went unpinned too. The Array of one class exists
# only once narrow_object_arrays decides it: the parameter is a boxed array
# the narrowing starts from the seed's class, and the signature's other types
# apply. Points passed narrow it; an array of another class keeps it boxed,
# as Ruby runs either. ext-test checks the C signatures of entries, which no
# call of the program types (test/ext/rbs_object_array.rb).
Point = Data.define(:x, :y)
Other = Data.define(:x)

module Geo
  #: (Array[Point], Integer) -> Float
  def self.sumx(pts, scale)
    t = 0.0
    pts.each { |p| t += p.x }
    t * scale
  end

  #: (Array[Other]) -> Float
  def self.other_sum(os)
    t = 0.0
    os.each { |o| t += o.x }
    t
  end
end

p Geo.sumx([Point.new(x: 1.0, y: 0.0), Point.new(x: 2.5, y: 0.0)], 2)
a = []
3.times { |i| a << Point.new(x: i * 1.5, y: 0.0) }
p Geo.sumx(a, 1)
p a.size
p Geo.other_sum([Other.new(x: 2.0), Other.new(x: 0.5)])
p Geo.other_sum([Point.new(x: 1.0, y: 2.0)])
