# A `**h` through a prepended forward without a keyword rest, into an empty initialize of the class's own: the forward's
# binding raises for a key it does not name, where nothing past it would.
module Sized
  def initialize(*a, size: 1) = super
end
class Array
  prepend Sized
  def initialize(*); end
end
h = { extra: 1 }
x = begin
  Array.new(5, **h)
rescue ArgumentError => e
  e.message
end
p x
h = { size: 2 }
p Array.new(5, **h)
