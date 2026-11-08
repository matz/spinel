# spinel: gc-stress
# A class held as a value (a Hash value, an Array element, a class variable)
# whose `.new` makes an Array subclass instance: the instance is boxed as its
# Array, so Array's own methods (<<, push, join) reach it through the boxed
# dispatch, and its class's own methods and its class still answer.
class SplitHeader < Array
  def join(separator = ", ")
    super
  end
end

class CookieHeader < Array
  def join(separator = "; ")
    super
  end
end

class Cells < Array
  def initialize(*cells)
    super(cells)
  end

  def total = sum
end

class Tagged < Array
  attr_reader :tag

  def initialize(tag, &blk)
    super()
    @tag = tag
    blk&.call(self)
  end
end

# a Hash of classes with a default class, read per key
HEADER_CLASSES = Hash.new(SplitHeader).update({ "cookie" => CookieHeader })
header = Hash.new([].freeze)
["host", "cookie", "cookie"].each do |f|
  header[f] = HEADER_CLASSES[f].new unless header.has_key?(f)
  header[f] << "v#{f}"
end
p header["host"].join
p header["cookie"].join
p header["cookie"].class

# an Array element, constructed with a splat
k = [Cells, SplitHeader].first
args = [1, 2, 3]
c = k.new(*args)
c << 4
p c.total
p c.size

# constructed with a block
t = { "t" => Tagged }["t"].new("x") { |a| a.push(7) }
t.push(8)
p t.tag
p t.to_a

# a class variable receiver
class Factory
  @@klass = SplitHeader
  def self.make = @@klass.new
end
m = Factory.make
m.push("a", "b")
p m.join
p m.is_a?(SplitHeader)

# an initialize that yields the new instance
class Gen < Array
  def initialize
    super()
    yield self
  end
end
g = [Gen].first.new { |a| a << "y" }
g << "z"
p g.join("+")
