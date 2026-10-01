# A global's or a class variable's String handed on to code that appends to
# it is that String, as in CRuby: a block a yield hands it to, and a method
# spliced with its block. The yield and the splice bound the block's or the
# method's parameter to a copy, and the append was lost.

def yy = yield($g)
$g = String.new
yy { |s| s << "b" }
p $g

def wrap(io) = (io << "w"; yield; nil)
$h = String.new
wrap($h) { }
p $h

class Y
  @@d = +"d"
  @@e = +"e"
  def self.yc = yield(@@d)
  def self.go = (yc { |s| s << "c" }; @@d)
  def self.wr = (wrap(@@e) { }; @@e)
end
p Y.go, Y.wr
