# A computed send on a value that a ternary or a method answers, beside nil,
# reaches the class method of the Class value it holds, as CRuby does. While
# the other branch (a `const_get` local) is not typed yet the value reads as
# nil, and the send must not be lowered over nil's methods alone.

class A
  def self.hello = "hi from #{name}"
  def greet = "inst"
end

def pick(n)
  k = Object.const_get(n)
  k == A ? k : nil
end

def pick_lt(n)
  k = Object.const_get(n)
  k.is_a?(Class) && k <= A ? k : nil
end

class Other; end

name_of = ->(s) { (s + "").to_sym }

puts pick("A").public_send(name_of.("hello"))
puts pick_lt("A").send(name_of.("hello"))
x = pick("A")
puts x.public_send(name_of.("hello"))
puts pick("A").new.public_send(name_of.("greet"))

# a receiver that is nil still answers nil's methods
p pick("Other").public_send(name_of.("nil?"))
p nil.send(name_of.("to_a"))
y = nil
p y.public_send(name_of.("inspect"))
begin
  y.public_send(name_of.("zork"))
rescue NoMethodError => e
  puts e.message
end
