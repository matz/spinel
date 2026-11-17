# A subclass of an Enumerable class answers map through Enumerable until
# the subclass defines its own map; its own map answers after that.

class Pair
  include Enumerable
  def each
    yield 1
    yield 2
  end
end

class Other
  def map = [:other]
end

class Sub < Pair
end

o = Sub.new
v = o.map { |x| x * 3 }
puts "before: #{v.inspect} #{v.class}"

class Sub
  def map = [:own]
end

v = o.map { |x| x * 3 }
puts "after: #{v.inspect} #{v.class}"
p Other.new.map
