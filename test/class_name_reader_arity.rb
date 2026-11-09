# Module#name, to_s and inspect take no argument. A class without a class
# method of its own of that name raises ArgumentError when a call gives one,
# also when another class with the same last name has `def self.name(x)`.
class T
  def self.name(x) = "top:#{x}"
end
module X
  class T; end
end

def check
  r = yield
  puts "ok #{r.inspect}"
rescue ArgumentError => e
  puts "ArgumentError: #{e.message}"
end

def arg
  puts "arg ran"
  1
end

check { T.name(1) }
check { X::T.name(1) }
check { X::T.to_s(2) }
check { X::T.inspect(3, 4) }
check { String.name(arg) }
check { Comparable.to_s(k: 1) }
check { String.name { 1 } }
k = X::T
check { k.name(5) }
[T, X::T].each { |cls| check { cls.name(6) } }
j = ARGV.size > 5 ? T : X::T
check { j.name(7) }
module Y
  class T; end
  check { T.name(8) }
end
check { X::T.name }
anon = Struct.new(:a)
check { anon.name(9) }
check { Struct.new(:b).name(10) }
check { anon.name }

# The bare spelling inside a class method: self is the class.
class F
  def self.a = (name(2) rescue $!.message)
  def self.b = (to_s(1, 2) rescue $!.message)
  class << self
    def c = (inspect(:x) rescue $!.message)
  end
  def self.d = [1].map { name(3) rescue $!.class }
  def self.e = "#{name}!"
end
class G
  def self.name(x) = "G#{x}"
  def self.f = name(4)
end
class H < G
  def self.g = name(5)
end
p F.a, F.b, F.c, F.d, F.e, G.f, H.g
