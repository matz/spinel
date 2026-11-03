# A Method object of a receiver that is nil raises NameError at the lookup,
# as `nil.method(:keep)` does in CRuby, where a Method bound to no object
# called the method on it (#10719, e4). And a reopened Array iterator
# called on an Array the inference has not typed yet (an ivar an empty
# literal seeds) runs the reopened method with the block, as on a typed
# one (e6).
class W
  def initialize(t) = @t = t
  def keep(x, memo) = (puts("#{@t}#{x}"); memo)
end
class Array
  def each_with_object(m) = (each { |x| yield(x, m) }; m)
end
class H
  attr_writer :w
  def initialize = (@w = W.new("a"); @arr = [3, 1])
  def run = p((@w = nil; @arr).each_with_object([], &@w.method(:keep)))
end
begin
  H.new.run
rescue NameError => e
  puts "#{e.class}: #{e.message}"
end

class E
  def initialize = (@w = W.new("e"); @arr = [])
  def fill = @arr << 7
  def run = p(@arr.each_with_object([], &@w.method(:keep)))
  def missing = p(@arr.each_with_object([], &@w.method(:nope)))
end
e = E.new
e.run
e.fill
e.run
begin
  e.missing
rescue NameError => x
  puts "#{x.class}: #{x.message}"
end
