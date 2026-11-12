# main's own to_s and inspect (its singleton's, which answer "main") come
# ahead of Object's table: an Object def, a Kernel def or a top-level def of
# the name answers other receivers, not a call on main.

p to_s
p inspect

module Kernel
  def to_s
    "k"
  end
end
p to_s
p nil.to_s

class Object
  def to_s
    "o"
  end

  def inspect
    "oi"
  end
end

p to_s
p self.to_s
p send(:to_s)
p public_send(:to_s)
p __send__(:to_s)
p inspect
p self.inspect
p public_send(:inspect)
p "#{self}"
p Object.new.to_s
p Object.new.inspect
p 1.to_s
p to_s.frozen?

3.times { |i| print to_s, i, "\n" }
p instance_eval { to_s }
p instance_exec(1) { |x| "#{inspect}#{x}" }
o = Object.new
def o.to_s = "own"
p o.instance_eval { to_s }

# before a later top-level def of the name, and after it
p to_s
def to_s
  "top"
end
p to_s
p Object.new.send(:to_s)
p method(:to_s).call
p method(:inspect).call
