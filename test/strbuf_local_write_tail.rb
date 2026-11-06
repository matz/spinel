# A method whose last expression writes a String handle to a local
# (`u = t`, `u ||= t`, `u &&= t`) answers that String, as `t` alone
# does: its callers take it as a String, keep its identity, and see
# later changes through it.

def write_fresh(ch)
  t = +"S"
  t << ch
  u = t
end

def write_param(ch)
  ch << "!"
  u = ch
end

def write_chain(ch)
  t = +"C"
  t << ch
  a = u = t
end

def or_write(ch)
  t = +"O"
  t << ch
  u = nil
  u ||= t
end

def and_write(ch)
  t = +"A"
  t << ch
  u = +"x"
  u << "y"
  u &&= t
end

q = +"q"
p or_write(q), and_write(q)
p [or_write(q).equal?(q), and_write(q) == "Aq"]
p write_fresh(q).equal?(q)
p [write_fresh(q).equal?(q), write_param(q).equal?(q)]
p write_fresh(q) == "Sq!"
p [write_fresh(q) == q]
r = write_param(q)
r << "?"
p q
f = write_fresh("z")
f << "#"
p f, f.frozen?
p write_chain("w")
p write_fresh("a").upcase, write_fresh("b").length
puts write_fresh("c")
p "#{write_fresh("d")}/#{write_chain("e")}"
x = (w = write_fresh("f"))
p x, w
v = [1, 2].map { |i| t = +"m"; t << i.to_s; u = t }
p v

class Item
  def initialize(n)
    @n = n
  end

  def inspect
    t = +"Item"
    t << @n.to_s
    u = t
  end

  def to_s
    t = +"item"
    t << @n.to_s
    u = t
  end

  def name
    t = +"n"
    t << @n.to_s
    u = nil
    u ||= t
  end

  def label(ch)
    t = +"L"
    t << ch
    u = t
  end
end

a = Item.new(1)
b = Item.new(2)
p a
p [a, b]
p({x: a})
puts a
print a, "\n"
puts [a, b].join(",")
puts "#{a}-#{b}"
puts a.inspect, [a, b].inspect
puts "#{a.name}:#{b.name}"
p [a.label(q).equal?(q), a.label(q) == "Lq!?", b.label("y")]

o = Object.new
def o.inspect
  t = +"I"
  t << "x"
  u = t
end
d = o.dup
p o
p [o]
p d.inspect.start_with?("#<Object")
c = o.clone
p c, c.inspect

class Base
  def name(ch) = (ch == "q" ? ch : "Z")
end
o2 = Base.new
c2 = o2.dup
def o2.name(ch)
  t = +"S"
  t << ch
  u = t
end
q2 = +"q"
p [c2.name(q2).equal?(q2), o2.name(q2).equal?(q2), o2.name("a")]
