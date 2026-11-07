# `Color::WHITE = v` at the top level, where Color is a constant holding
# `Data.define(...)` or `Struct.new(...)` and no `class Color` body exists,
# wrote a constant named only WHITE. A `Tex::WHITE` elsewhere then shared its
# one type: the program read an Integer where it wanted a Color, or answered
# the wrong value without an error.
Color = Data.define(:red, :green, :blue, :alpha) do
  def self.fade = Color::WHITE.red / 2

  # a read through the class value, in an instance and in a class method
  def white = self.class::WHITE
  def self.base = self::WHITE
end
Color::WHITE = Color.new(1.0, 1.0, 1.0, 1.0)

module Tex
  WHITE = 7

  def self.layer = WHITE + 1
end

# a holder written before the module that has the same constant name
Pair = Struct.new(:left) do
  def size = self.class::SIZE
end
Pair::SIZE = 1.5

module Grid
  SIZE = 10

  def self.cells = SIZE * 2
end

# two holders that each get the same constant name
Paint = Data.define(:name)
Paint::WHITE = Paint.new("white")

class User
  def layer_of(layer)
    layer + 1
  end

  def pick(color)
    color.red
  end

  def label
    Paint::WHITE.name
  end

  def size_sum
    Pair::SIZE + Grid::SIZE
  end
end

user = User.new
p user.layer_of(Tex::WHITE)
p user.pick(Color::WHITE)
p user.label
p user.size_sum
p Tex.layer
p Color.fade
p Grid.cells
p Pair::SIZE + 1.0
p Tex::WHITE + 1
p Color.new(0.0, 0.0, 0.0, 0.0).white.red
p Color.base.red
p Pair.new(0).size

# a subclass of the holder that writes the same name: its own value wins,
# and the holder's own (a Float beside the subclass's Integer) is still its
Rim = Struct.new(:size) do
  def self.limit = self::LIMIT
end
Rim::LIMIT = 1.5

module Cap
  LIMIT = 10
end

class BigRim < Rim
  LIMIT = 99
end

p BigRim.limit
p Rim.limit
p Cap::LIMIT
