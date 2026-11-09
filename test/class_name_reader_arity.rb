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
