# A lazy chain written with `&.` at every link, on a receiver that may be
# nil, answers nil for nil and the forced values otherwise: on a typed
# Array, a boxed value, a Hash, an Enumerable object of the program and
# its subclass, held in a local, assigned, returned from a method and two
# in one statement. A block with a side effect runs only past the guard.

class Bag
  include Enumerable
  def initialize(*v) = (@v = v)
  def each(&b) = @v.each(&b)
end
class Sub < Bag; end

def note(v) = (puts "note #{v}"; v)
def arr(f) = f ? [3, 1, 2] : nil
def boxed(f) = f ? [3, "x", 2] : (f.nil? ? 1 : nil)
def hsh(f) = f ? { a: 1, b: 2 } : nil
def bag(f) = f ? Sub.new(5, 6, 7) : nil
def tail(n) = n&.lazy&.map { |x| x + 1 }&.first(2)

[false, true].each do |f|
  a = arr(f)
  p a&.lazy&.first(1)
  p a&.to_a&.lazy&.first(1)
  p a&.lazy&.map { |x| note(x) * 2 }&.select { |x| x > 2 }&.first(1)
  p a&.lazy&.reject(&:odd?)&.to_a
  p a&.lazy&.map { |x| x + 1 }&.force
  p a&.lazy&.map { |x| x * 2 }&.first
  p a&.lazy&.flat_map { |x| [x, x] }&.first(3)
  p a&.lazy&.filter_map { |x| x * 2 if x > 1 }&.to_a
  p a&.lazy&.take_while { |x| x > 2 }&.to_a
  p a&.lazy&.with_index&.map { |x, i| x * i }&.to_a
  p a&.lazy&.each_slice(2)&.first
  b = boxed(f)
  p b&.lazy&.first(2)
  h = hsh(f)
  p h&.lazy&.map { |k, v| [v, k] }&.to_a
  g = bag(f)
  p g&.lazy&.first(1)
  p g&.lazy&.map { |x| x * 10 }&.first(2)
  p g&.lazy&.first
  r = a&.lazy&.select { |x| x > 1 }&.to_a
  p r
  p tail(a), tail(g)
  p [a&.lazy&.first(1), g&.lazy&.force]
  puts "s: #{a&.lazy&.take(2)&.to_a.inspect}"
end

# A plain `.` after the chain, or a forcing call the lazy emitters do not
# fuse, leaves a guard at each `&.` link: on nil the chain answers what
# the call after it answers on nil.
def chk
  p yield
rescue NoMethodError => e
  puts "NoMethodError #{e.name}"
end
n = arr(false)
chk { n&.lazy&.map { |x| x }.nil? }
chk { n&.lazy&.map { |x| x * 2 }.first }
chk { n&.lazy&.map { |x| x * 2 }.first(2) }
chk { r = []; n&.lazy&.map { |x| x + 1 }.each { |x| r << x }; r }
chk { r = []; n&.lazy&.map { |x| x * 2 }&.each { |x| r << x }; r }
chk { n&.lazy&.map { |x| x * 2 }&.eager&.first(2) }
