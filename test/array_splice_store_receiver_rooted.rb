# spinel: gc-minor
# spinel: gc-stress
# A slice store `a[i, n] = v` / `a[r] = v` keeps its Array alive while the index, the length
# and the value run, when they drop the last other reference to it.
def churn(n)
  k = 0
  n.times { |i| k += ("y" * 50 + i.to_s).size }
  k
end

class Box
  attr_accessor :s, :i, :f, :p

  def initialize
    @s = [+"p", +"q", +"r"]
    @i = [1, 2, 3]
    @f = [1.5, 2.5, 3.5]
    @p = [1, "x", :y]
  end

  def swap!
    @s = [+"s"]
    @i = [9]
    @f = [9.5]
    @p = [7]
    churn(3000)
  end

  def show
    puts [@s, @i, @f, @p].inspect
  end
end

bx = Box.new
bx.s[0, 1] = (bx.swap!; "w" * 40)
bx.show

bx = Box.new
bx.i[0, 1] = (bx.swap!; 5)
bx.show

bx = Box.new
bx.f[0, 1] = (bx.swap!; 5.5)
bx.show

bx = Box.new
bx.p[0, 1] = (bx.swap!; "z" * 40)
bx.show

bx = Box.new
bx.i[(bx.swap!; 0), 1] = 6
bx.show

bx = Box.new
bx.p[0, (bx.swap!; 1)] = "q" * 40
bx.show

bx = Box.new
bx.i[0..(bx.swap!; 0)] = 7
bx.show

bx = Box.new
v = (bx.i[0, 1] = (bx.swap!; 5))
p v
bx.show

bx = Box.new
v = (bx.s[0, 1] = (bx.swap!; "w" * 40))
p v
bx.show

bx = Box.new
v = (bx.p[0, 1] = (bx.swap!; "z" * 40))
p v
bx.show
