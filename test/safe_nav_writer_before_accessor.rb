# A safe-navigation write of an attribute raises NoMethodError while the
# class has no writer, and stores once an attr_accessor of the name has run.

class Box
  def initialize = (@x = 1)
end

class Other
  attr_accessor :x
end

def pick(f, o) = f ? o : nil

b = Box.new
q = pick(true, b)
z = pick(false, b)
begin
  q&.x = 4
  puts "before: stored"
rescue NoMethodError => e
  puts "before: #{e.class}"
end
p(z&.x = 4)

class Box
  attr_accessor :x
end

v = (q&.x = 5; q&.x)
puts "after: #{v.inspect}"
p(z&.x = 6)
o = Other.new
o.x = 2
p o.x
