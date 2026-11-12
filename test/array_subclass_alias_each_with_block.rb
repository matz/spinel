# An Array subclass's aliases of each and size beside its own: the alias
# called with a block forwards it to Array's each, and the one called
# without (size) is Array's own call, with no block argument.
class Log < Array
  alias_method :raw_each, :each
  alias_method :raw_size, :size
  def each(&b) = raw_each { |x| b.call(x * 10) }
  def size = raw_size * 10
end
l = Log.new([1, 2])
l.raw_each { |x| p x }
l.each { |x| p x }
p l.size
p l.raw_size
