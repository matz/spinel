# `Cls.send(:m)` reaches the class's own class method m before a top-level
# `def m`, which a send can also reach (it is Object's private method). The
# retargeted call asked only an instance receiver whether it owned the name,
# so a class receiver took the top-level def: the wrong method, and a C
# value of the wrong type.
def k(a:) = "top #{a}"
def twice(x) = x * 2

class B
  def self.k(a:) = "B.k #{a}"
  def self.size = 3
end

class C < B
end

class D
  def self.make = self
end

p B.send(:k, a: 1)
p C.send(:k, a: 2)
p B.__send__(:k, a: 3)
p B.send(:size)
p D.send(:k, a: 4)
p B.send(:twice, 5)
p D.make.send(:k, a: 6)
