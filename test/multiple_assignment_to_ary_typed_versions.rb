# A multiple assignment from an object whose to_ary versions answer
# different things: Arrays of other element types, nil (the object is
# taken whole) and a non-Array (TypeError).
class K
  def to_ary = [1, 2]
  def inspect = "#<K>"
end

def pair(o)
  a, b = o
  [a, b]
end

def rest(o)
  a, *r = o
  [a, r]
end

class Box
  def initialize(o)
    @a, @b = o
  end

  def to_s = "#{@a.inspect} #{@b.inspect}"
end

k = K.new
p pair(k), rest(k)
puts Box.new(k)
class K; def to_ary = ["x", :y, 3.5]; end
p pair(k), rest(k)
puts Box.new(k)
class K; def to_ary = nil; end
p pair(k), rest(k)
puts Box.new(k)
class K; def to_ary = 42; end
[-> { pair(k) }, -> { rest(k) }, -> { Box.new(k) }].each do |f|
  begin
    f.call
  rescue TypeError => e
    puts "TypeError: #{e.message}"
  end
end
