# An entry's parameter holds what the host hands it, which an earlier entry
# answered: the objects in an Array field of that parameter are the ones the
# program made, so a call on one of them dispatches to their class. The
# parameter held no class, as a method nothing calls holds none, and the
# call on an element was a NoMethodError for a method the class has.
Point = Data.define(:x, :y)
Grid = Data.define(:name, :slots)
class Item
  attr_reader :done
  def initialize(d) = @done = d
end
class Box
  attr_reader :items
  def initialize = @items = [Item.new(true), nil, Item.new(true), Item.new(false)]
end
module ExtFieldElems
  #: () -> Grid
  def self.grid = Grid.new(name: "g", slots: [Point.new(x: 1, y: 2), nil, Point.new(x: 3, y: 4)])
  #: (Grid) -> Integer
  def self.sum_x(g)
    t = 0
    g.slots.each { |p| t += p.x if p }
    t
  end
  #: () -> Box
  def self.box = Box.new
  #: (Box) -> Integer
  def self.count_done(b) = b.items.count { |i| i && i.done }
end
