# A class method body's definer (attr_accessor, define_method) or visibility word is a version of its class's
# method, also when another class's method body has defined a method before this class is opened: the index of
# the entries that bar a body's version is made again once the header of a class its first making read as not
# yet recorded opens. Each class here has names of its own, so that no other class's entry bars them.

# the other class's maker is an attr_accessor, run before the class is opened
class C1
  def self.mkc = attr_accessor(:a1)
end
C1.mkc
o1 = C1.new
o1.a1 = 1
p o1.a1

class D1
  def b1 = :def
  def self.mk = attr_accessor(:b1)
  def self.hide = private(:b1)
end
d1 = D1.new
D1.mk
d1.b1 = 2
p d1.b1
D1.hide
p((d1.b1 rescue :nme_private))
p d1.send(:b1)

# the other class's maker is a define_method, and the class's has no word
class C2
  def self.mkc = define_method(:a2) { 1 }
end
C2.mkc
p C2.new.a2

class D2
  def b2 = :def
  def self.mk = attr_accessor(:b2)
end
d2 = D2.new
D2.mk
d2.b2 = 3
p d2.b2

# a define_method in the class's own body
class C3
  def self.mkc = define_method(:a3) { 1 }
end
C3.mkc
p C3.new.a3

class D3
  def b3 = :def
  def self.mk = define_method(:b3) { :made }
  def self.hide = private(:b3)
end
d3 = D3.new
D3.mk
p d3.b3
D3.hide
p((d3.b3 rescue :nme_private))
p d3.send(:b3)

# the other class's maker is a def in a method body
class C4
  def self.mkc = (def made = 4)
end
C4.mkc
p C4.new.made

class D4
  def b4 = :def
  def self.mk = attr_accessor(:b4)
  def self.hide = private(:b4)
end
d4 = D4.new
D4.mk
d4.b4 = 5
p d4.b4
D4.hide
p((d4.b4 rescue :nme_private))
p d4.send(:b4)
