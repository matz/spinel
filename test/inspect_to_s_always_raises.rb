# A program's own #inspect or #to_s whose body only raises: p, puts,
# interpolation and the render of a container that holds the object
# raise the method's error (they failed the C build or printed #<K:0x...>).
class Bar
  def inspect
    raise ArgumentError, "boom"
  end
  def to_s = raise(TypeError, "ts")
end
class Baz < Bar
  def inspect = "baz!"
end
module Loud
  def inspect = raise("mod")
end
class Q; include Loud; end
class K
  def self.inspect = raise("cls")
  def self.to_s = raise("clsts")
end
S = Struct.new(:a) do
  def inspect = raise("st")
end
def show
  yield
rescue => e
  puts "#{e.class}: #{e.message}"
end
show { p Bar.new }
show { puts "#{Bar.new}" }
show { puts Bar.new.to_s.size }
show { p Baz.new }
show { p [Baz.new, Bar.new] }
show { p Q.new }
show { p K }
show { puts "#{K}" }
show { p S.new(1) }
show { p({k: Bar.new}) }
show { x = [Bar.new, 1, "s"].first; p x }
show { x = [Bar.new, 1].first; puts "#{x}" }
show { s = Bar.new.inspect; puts s.upcase }
show { puts Bar.new }
show { print Bar.new, "\n" }
show { $stdout.write(Bar.new) }
puts "end"
