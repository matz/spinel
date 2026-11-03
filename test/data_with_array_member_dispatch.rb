# Data#with stores elements into an Array member; a method reads them back
# through the member and calls theirs. Each copy is made from an instance
# `new` built, which holds no element yet.
Pt = Data.define(:x)
class Pt
  def fade = "f#{x}"
end

Pool = Data.define(:live)
class Pool
  def emit(x) = with(live: [*live, Pt.new(x)])
  def paint = live.each { |pt| puts pt.fade }
end

Pool.new(live: []).emit(1).paint

# the Array built by `+`, the element class nested and defined after its
# first use
Swarm = Data.define(:live)
class Swarm
  def burst(count) = with(live: live + Array.new(count) { |i| Dot.new(x: i) })
  def paint = live.each { |d| puts d.label }

  Dot = Data.define(:x)
  class Dot
    def label = "dot #{x}"
  end
end

Swarm.new(live: []).burst(2).paint

# filter_map over the member
Bag = Data.define(:items)
class Bag
  def keep = with(items: items.filter_map { |i| Item.new(n: i) if i.odd? })
  def show = items.each { |it| puts it.name }
end

Item = Data.define(:n)
class Item
  def name = "item #{n}"
end

Bag.new(items: [1, 2, 3]).keep.show

# the elements come in a `**` Hash, the last of several
Spark = Data.define(:x)
class Spark
  def glow = "spark #{x}"
end
Flare = Data.define(:live, :n)
class Flare
  def emit(x)
    head = {n: 1}
    tail = {live: [*live, Spark.new(x)]}
    with(**head, **tail)
  end
  def paint = live.each { |s| puts s.glow }
end

Flare.new(live: [], n: 0).emit(2).paint

# a custom initialize hands a keyword to another member through super
Bead = Data.define(:x)
class Bead
  def shine = "bead #{x}"
end
Pair = Data.define(:a, :b)
class Pair
  def initialize(a:, b:) = super(a: b, b: a)
  def emit(x) = with(a: [Bead.new(x)])
  def paint = b.each { |bd| puts bd.shine }
end

Pair.new(a: [], b: []).emit(3).paint

# a bare super hands on the keywords
Seed = Data.define(:x)
class Seed
  def sprout = "seed #{x}"
end
Row = Data.define(:live, :n)
class Row
  def initialize(live:, n: 0)
    super
  end
  def emit(x) = with(live: [*live, Seed.new(x)])
  def paint = live.each { |sd| puts sd.sprout }
end

Row.new(live: []).emit(4).paint

# the receiver is a lambda's parameter, which may be anything
Drop = Data.define(:x)
class Drop
  def fall = "drop #{x}"
end
Cloud = Data.define(:live)
class Cloud
  def paint = live.each { |dr| puts dr.fall }
end

grow = ->(c) { c.with(live: [Drop.new(5)]) }
grow.call(Cloud.new(live: [])).paint
