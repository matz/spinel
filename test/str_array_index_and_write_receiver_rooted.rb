# spinel: gc-minor
# spinel: gc-stress
# An index-and/or-write on a String Array keeps its Array alive while the
# index and the value run, when they drop the last other reference to it.
def churn(n)
  k = 0
  n.times { |i| k += ("y" * 50 + i.to_s).size }
  k
end

class Box
  attr_accessor :a, :h, :n
  def initialize
    @a = [+"p", +"q", +"r"]
    @n = [+"p", ENV["SPINEL_NO_SUCH_VAR"]]
    @h = { "k" => +"v", "j" => +"w" }
  end

  def swap!
    @a = [+"s"]
    @n = [+"s"]
    @h = {}
    churn(3000)
  end
end

bx = Box.new
v = (bx.a[1] &&= (bx.swap!; "w" * 40))
puts v
puts bx.a.inspect

bx = Box.new
v = (bx.a[(bx.swap!; 0)] ||= "x" * 40)
puts v
puts bx.a.inspect

bx = Box.new
v = (bx.a[(bx.swap!; 2)] &&= "y" * 40)
puts v
puts bx.a.inspect

bx = Box.new
v = (bx.n[1] ||= (bx.swap!; "n" * 40))
puts v
puts bx.n.inspect

bx = Box.new
v = (bx.h["k"] &&= (bx.swap!; "z" * 40))
puts v
puts bx.h.inspect
