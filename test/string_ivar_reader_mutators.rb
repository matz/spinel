# slice!, []=, insert and setbyte through a reader, and the same four
# inside the class: they used to disqualify the ivar from the shared handle,
# so a later `<<` through the reader landed in a copy.

class C1
  attr_reader :name
  def initialize = @name = +"abc"
end
def setbyte_slice_through_reader
  c = C1.new
  c.name.setbyte(0, 90)
  p c.name
  c.name.slice!(0)
  p c.name
end
setbyte_slice_through_reader

class C2
  attr_reader :name
  def initialize = @name = +"abcd"
end
def four_through_reader
  c = C2.new
  c.name.setbyte(0, 90)
  p c.name
  p c.name.slice!(0)
  p c.name
  c.name[0] = "X"
  p c.name
  c.name.insert(1, "-")
  p c.name
end
four_through_reader

class C3
  attr_reader :name
  def initialize = @name = +"abc"
  def poke = @name.setbyte(0, 90)
  def cut = @name.slice!(0)
  def put = (@name[0] = "Q")
  def ins = @name.insert(1, "-")
end
def four_inside_the_class
  c = C3.new
  c.poke
  p c.name
  c.name << "!"
  p c.name
  p c.cut
  p c.name
  c.put
  c.ins
  p c.name
  x = c.name
  c.name << "?"
  p x
end
four_inside_the_class

class C4
  attr_reader :name
  def initialize(s) = @name = s
end
def frozen_through_reader
  c = C4.new(+"ab")
  c.name.slice!(0)
  p c.name
  d = C4.new("xy".freeze)
  begin
    d.name[0] = "Q"
  rescue FrozenError => e
    p e.class
  end
  begin
    d.name.slice!(0)
  rescue FrozenError => e
    p e.class
  end
  p d.name
end
frozen_through_reader
