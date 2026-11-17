# A method reached only by a send of a computed name is entered, so a reader
# it calls on an untyped parameter still counts for the narrowing.
class Item
  attr_reader :n
  def initialize(n) = @n = n
end

class Bag
  def initialize = @items = [Item.new(1), Item.new(2)]
  def items = @items
end

class Proxy
  def hidden(s) = s.items.map { |i| i.n }
end

p Proxy.new.public_send(["hid", "den"].join, Bag.new)
