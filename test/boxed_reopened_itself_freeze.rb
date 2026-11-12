# A boxed receiver calls a reopened builtin class's own itself or freeze,
# as a typed receiver does. The builtin's answer was used for every element.

class Integer
  def itself = :int_itself
  def freeze = :int_freeze
end

class String
  def itself = :str_itself
  def freeze = :str_freeze
end

x = [1, "s", :a]
p x.map { |v| v.itself }
p x.map { |v| v.freeze }
p x.map(&:itself)
p x.each_with_index.map { |v, i| v.freeze }
p x.first.freeze, x[1].itself

module Tagged
  def itself = :tagged
end
class Float
  include Tagged
end
p [2.5, :b].map { |v| v.itself }

# a class without its own freeze keeps the builtin one
y = [[1], String.new("t"), 3]
z = y.map { |v| v.freeze }
p z[0].frozen?, y[0].frozen?
begin
  z[0] << 2
rescue FrozenError => e
  p e.class
end

# a program class's own itself that calls super leaves the reopened
# Integer#itself answering for a boxed Integer
class Wrapped
  def itself = [:wrapped, super.class]
end
p [1, Wrapped.new].map { |v| v.itself }
