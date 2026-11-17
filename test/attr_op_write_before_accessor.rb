# An attribute op-write that runs before the class has the accessor calls a
# reader that is not defined yet: NoMethodError names the reader, and the
# value is never evaluated. Once attr_accessor has run, the same write
# reads and stores.
class Box
  def initialize = (@x = 1)
end
class Bare
  def initialize = (@x = 1)
end
def side
  puts "side"
  1
end
o = Box.new
begin
  o.x += side
  p o.x
rescue NoMethodError => e
  puts "before: #{e.message}"
end
begin
  o.x *= 3
  p o.x
rescue NoMethodError => e
  puts "before: #{e.message}"
end
o.x += 1 if ARGV.size > 3
begin
  Bare.new.x -= 1
rescue NoMethodError => e
  puts "never: #{e.message}"
end
class Box
  attr_accessor :x
end
o.x += side
p o.x
o.x *= 10
p o.x
