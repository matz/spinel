# spinel: gc-minor
# spinel: gc-stress
# An index-and/or-write on an Integer, Float or general Array keeps its Array alive while
# the index and the value run, when they drop the last other reference to it.
def churn(n)
  k = 0
  n.times { |i| k += ("y" * 50 + i.to_s).size }
  k
end

class Box
  attr_accessor :i, :f, :p, :in_, :fn, :pn

  def initialize
    @i = [1, 2, 3]
    @f = [1.5, 2.5, 3.5]
    @p = [1, "x", :y]
    @in_ = [1, nil, 3]
    @fn = [1.5, nil, 3.5]
    @pn = [1, nil, :y]
  end

  def swap!
    @i = [9]
    @f = [9.5]
    @p = [7]
    @in_ = [9]
    @fn = [9.5]
    @pn = [7]
    churn(3000)
  end

  def show
    puts [@i, @f, @p, @in_, @fn, @pn].inspect
  end
end

bx = Box.new
v = (bx.i[1] &&= (bx.swap!; 5))
p v
bx.show

bx = Box.new
v = (bx.f[1] &&= (bx.swap!; 5.5))
p v
bx.show

bx = Box.new
v = (bx.p[1] &&= (bx.swap!; "z" * 40))
p v
bx.show

bx = Box.new
v = (bx.in_[1] ||= (bx.swap!; 5))
p v
bx.show

bx = Box.new
v = (bx.fn[1] ||= (bx.swap!; 5.5))
p v
bx.show

bx = Box.new
v = (bx.pn[1] ||= (bx.swap!; "z" * 40))
p v
bx.show

bx = Box.new
v = (bx.in_[(bx.swap!; 1)] ||= 6)
p v
bx.show

bx = Box.new
v = (bx.i[(bx.swap!; 2)] &&= 6)
p v
bx.show

bx = Box.new
v = (bx.p[(bx.swap!; 1)] &&= "q" * 40)
p v
bx.show
