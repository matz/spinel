# A Struct's or Data's own initialize evaluates its defaults on the object
# being built, as any class's does. The construction rendered a default that
# reads self at the call site, on the caller's self: no self at the top level,
# and the caller's object inside a method.

Kind = Struct.new(:kind) do
  def initialize(kind = self.class)
    super(kind)
  end
end

class SubKind < Kind
end

puts Kind.new.kind.name
puts SubKind.new.kind.name
puts Kind.new(SubKind).kind.name

Point = Struct.new(:x, :origin) do
  def initialize(x, origin = self)
    super(x, origin)
  end
end

class Maker
  def make = Point.new(1)
end

pt = Point.new(0)
puts pt.origin.equal?(pt)
m = Maker.new.make
puts m.origin.equal?(m)
puts m.origin.class.name
puts Point.new(5, :given).origin == :given

class Tagged < Struct.new(:tag)
  def initialize(tag = self.class.name)
    super(tag)
  end
end

class SubTagged < Tagged
end

puts Tagged.new.tag
puts SubTagged.new.tag

Coord = Data.define(:x, :kind) do
  def initialize(x:, kind: self.class)
    super(x: x, kind: kind)
  end
end

class Coord3 < Coord
end

puts Coord.new(x: 1).kind.name
puts Coord3.new(x: 2).kind.name

# Allocating before initialize must preserve the Data constructor's freeze,
# including when the keyword with a self-reading default was supplied.
puts Coord.new(x: 1).frozen?
puts Coord.new(x: 1, kind: Coord).frozen?
puts Coord3.new(x: 2).frozen?
puts Coord3.new(x: 2, kind: Coord).frozen?

coord = Coord.new(x: 1)
updated = coord.with(x: 2)
puts updated.class.name
puts updated.kind.name
puts updated.frozen?
puts coord.with.equal?(coord)
puts coord.with.frozen?
puts Coord3.new(x: 2).with(x: 3).frozen?

[Coord, Coord3].each do |klass|
  value = klass.new(x: 3)
  puts value.kind.name
  puts value.frozen?
end

PosCoord = Data.define(:kind) do
  def initialize(kind = self.class)
    super(kind: kind)
  end
end

class PosCoord3 < PosCoord
end

puts PosCoord.new.kind.name
puts PosCoord.new.frozen?
puts PosCoord.new(PosCoord3).frozen?
puts PosCoord3.new.kind.name
puts PosCoord3.new.frozen?
puts PosCoord3.new(PosCoord).frozen?

# Member-wise construction still freezes, and other classes stay mutable.
PlainCoord = Data.define(:kind)
puts PlainCoord.new(PlainCoord).frozen?
puts PlainCoord.new(kind: PlainCoord).frozen?
puts Kind.new.frozen?
puts SubKind.new.frozen?

class MutableKind
  attr_reader :kind
  def initialize(kind: self.class)
    @kind = kind
  end
end

class MutableChild < MutableKind
end

puts MutableKind.new.kind.name
puts MutableKind.new.frozen?
puts MutableChild.new.kind.name
puts MutableChild.new.frozen?
