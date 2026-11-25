# The singleton class of a class made at run time is its own, one per
# class, and names the class it belongs to.

def mk = Class.new { def self.x = 1 }
a = mk
b = mk
s = a.singleton_class
p s.name, s.singleton_class?, s == a.singleton_class, s == b.singleton_class, s.class
h = { s => 1, b.singleton_class => 2 }
p h.size, h[a.singleton_class]
p [s].map(&:to_s).first.start_with?("#<Class:#<Class:0x")
p a.singleton_class? , s.to_s == s.inspect
B = mk
p B.singleton_class
