# A method that is not captured keeps its lane beside a captured method of
# the same name: another class's, a namespaced class's, and the class
# method beside the captured instance method. Only the methods the
# `method(:echo)` captures resolve to take the boxed argument.
class Kept
  def echo(value) = value + 1
end
class Captured
  def echo(value) = value
end
class Item
  def echo(value) = value + 1
end
module Box
  class Item
    def echo(value) = value
  end
end
class Shape
  def self.echo(value) = value + 1
end
class Shape
  def echo(value) = value
end
def adapter(box)
  callback = box[:load]
  proc { |value| callback.call(value) }
end
p Kept.new.echo(0), Captured.new.echo(0), Item.new.echo(0), Box::Item.new.echo(0), Shape.echo(0), Shape.new.echo(0)
[Captured.new.method(:echo), Box::Item.new.method(:echo), Shape.new.method(:echo)].each do |m|
  f = adapter({load: m})
  p f.call(1.5), f.call("s")
end
p Kept.new.echo(4), Item.new.echo(4), Shape.echo(4)
