# An instance accessor and a def of one name in one class answer by which is
# live at each call, in either order: the call plan gives each accessor
# version of such a history a method of its own, and a plain accessor stays
# a field read.
class AttrFirst
  attr_reader :x
  def initialize; @x = :attr; end
end
a = AttrFirst.new
p a.x
class AttrFirst
  def x = :def
end
p a.x

class DefFirst
  def initialize; @x = :attr; end
  def x = :def
end
d = DefFirst.new
p d.x
class DefFirst
  attr_reader :x
end
p d.x

class WriterFirst
  attr_accessor :x
end
w = WriterFirst.new
w.x = 1
p w.x
class WriterFirst
  def x=(v); @x = v * 10; end
end
w.x = 2
p w.x

class SelfRead
  attr_reader :v
  def initialize; @v = 1; end
  def now = v
end
s = SelfRead.new
p s.now
class SelfRead
  def v = 2
end
p s.now

class Aliased
  attr_reader :x
  alias y x
  def initialize; @x = 1; end
  def x = 2
end
al = Aliased.new
p al.x, al.y
