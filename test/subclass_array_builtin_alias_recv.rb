# An alias that captured Array's method in an Array subclass, called on a
# receiver, is Array's size, not the class's own `size` the alias's name
# resolved to by name; inside the class it is Array's too.
class Log < Array
  alias_method :raw_size, :size
  def size = raw_size * 10
end
l = Log.new([1, 2])
p l.raw_size
p l.size
