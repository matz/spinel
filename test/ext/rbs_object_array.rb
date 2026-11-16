# Entries whose inline signatures name an Array of a program class. No call
# of the program types an entry's parameters, so the signature is all that
# does. `Array[Point]` named no type Spinel could pin, and the signature was
# dropped whole: `length` took an sp_RbVal, and `pts` an sp_RbVal for its
# Integer. Now the other types apply, the Array parameter is a boxed array
# narrow_object_arrays starts from Point, and its uses here allow the
# sp_PtrArray the host builds. test/ext/rbs_object_array_host.c passes one.
Point = Data.define(:x, :y)

module ExtGeo
  #: (Integer) -> Array[Point]
  def self.pts(n) = Array.new(n) { |i| Point.new(x: i * 3.0, y: i * 4.0) }

  #: (Array[Point]) -> Float
  def self.length(pts)
    t = 0.0
    i = 1
    while i < pts.size
      a = pts[i - 1]
      b = pts[i]
      t += Math.sqrt((b.x - a.x) ** 2 + (b.y - a.y) ** 2)
      i += 1
    end
    t
  end
end
