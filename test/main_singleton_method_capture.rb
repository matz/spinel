# `method(:to_s)` on main captures its singleton's to_s, which answers
# "main", ahead of an Object def of the name
m = method(:to_s)
p m.call
p method(:inspect).call
p [1].map { method(:to_s).call }

class Object
  def to_s = "o"
  def inspect = "oi"
end

p method(:to_s).call
p self.method(:inspect).call
p m.call
p instance_exec { method(:to_s).call }
p Object.new.method(:to_s).call
o = Object.new
p o.instance_exec { method(:inspect).call }
# a block a method of the program runs may run on another object
def on(x, &b) = x.instance_eval(&b)
p on(Object.new) { to_s }
