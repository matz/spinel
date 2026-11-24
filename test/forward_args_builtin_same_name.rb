# A `...` forwarder whose call on another receiver has the forwarder's own
# name reaches the builtin's method of the name: with no argument, a
# positional one, a block, a leading parameter, and from several classes.
require "json"

class Body
  def as_json = {a: 1}
  def to_s(...) = as_json.to_s(...)
  def to_json(...) = as_json.to_json(...)
end

class Items
  def as_json = [1, 2]
  def to_json(...) = as_json.to_json(...)
end

class List
  def initialize = @a = [3, 1, 2]
  def first(...) = @a.first(...)
  def each(...) = @a.each(...)
end

class Table
  def initialize = @h = {a: 1}
  def fetch(...) = @h.fetch(...)
end

class Label
  def initialize = @s = "abc"
  def center(width, ...) = @s.center(width, ...)
end

puts Body.new.to_s
puts Body.new.to_json
puts Items.new.to_json
p List.new.first(2)
List.new.each { |x| p x }
p Table.new.fetch(:b) { |k| k }
p Table.new.fetch(:a) { |k| k }
p Label.new.center(7, "*")
