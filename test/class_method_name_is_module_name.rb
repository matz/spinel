# `name` in a class method answers Module#name: the class's own name, not
# the class's to_s, for the class and for a subclass that inherits the
# method. A program's own `def self.name` called the same way answers its
# String as it is, not a frozen copy.
class Shape
  def self.to_s = "a shape"
  def self.label = self.name
  def self.bare = name
end
class Circle < Shape; end
p Shape.label, Circle.label, Shape.bare, Circle.bare, Shape.to_s

class Tag
  def self.name = +"tag"
  def self.own = name
end
s = Tag.own
p s.frozen?
s << "!"
p s
