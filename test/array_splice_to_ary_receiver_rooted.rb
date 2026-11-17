# spinel: gc-minor
# spinel: gc-stress
# A slice store whose right side is an object with a program to_ary keeps its Array alive while that
# conversion runs: the conversion replaces the Array the receiver read (an ivar, a closure's local)
# and allocates.
def churn(n)
  k = 0
  n.times { |i| k += ("y" * 50 + i.to_s).size }
  k
end
class Box
  attr_accessor :i, :f, :s, :p
  def initialize
    @i = [1, 2, 3]; @f = [1.5, 2.5, 3.5]; @s = [+"p", +"q", +"r"]; @p = [1, "x", :y]
  end
  def swap!
    @i = [9]; @f = [9.5]; @s = [+"s"]; @p = [7]
    churn(3000)
  end
end
class Ti
  def initialize(b); @b = b; end
  def to_ary; @b.swap!; [5]; end
end
class Tf
  def initialize(b); @b = b; end
  def to_ary; @b.swap!; [5.5]; end
end
class Ts
  def initialize(b); @b = b; end
  def to_ary; @b.swap!; [+"w"]; end
end
class Tp
  def initialize(b); @b = b; end
  def to_ary; @b.swap!; ["z", 3]; end
end
class Run
  def initialize(b); @b = b; end
  def st_i(o) = (@b.i[0, 1] = o; @b.i)
  def st_f(o) = (@b.f[0, 1] = o; @b.f)
  def st_s(o) = (@b.s[0, 1] = o; @b.s)
  def st_p(o) = (@b.p[0, 1] = o; @b.p)
  def v_i(o) = (r = (@b.i[0, 1] = o); [r.class, @b.i])
  def v_s(o) = (r = (@b.s[0, 2] = o); [r.class, @b.s])
  def rg_i(o) = (@b.i[1..1] = o; @b.i)
end
bx = Box.new; p Run.new(bx).st_i(Ti.new(bx))
bx = Box.new; p Run.new(bx).st_f(Tf.new(bx))
bx = Box.new; p Run.new(bx).st_s(Ts.new(bx))
bx = Box.new; p Run.new(bx).st_p(Tp.new(bx))
bx = Box.new; p Run.new(bx).v_i(Ti.new(bx))
bx = Box.new; p Run.new(bx).v_s(Ts.new(bx))
bx = Box.new; p Run.new(bx).rg_i(Ti.new(bx))

class TiP
  def initialize(pr); @pr = pr; end
  def to_ary; @pr.call; churn(3000); [5]; end
end
class TsP
  def initialize(pr); @pr = pr; end
  def to_ary; @pr.call; churn(3000); [+"w"]; end
end
def cell
  arr = [1, 2, 3]
  o = TiP.new(-> { arr = [9] })
  arr[0, 1] = o
  arr
end
p cell
def cell_s
  arr = [+"p", +"q"]
  o = TsP.new(-> { arr = [+"s"] })
  arr[0, 1] = o
  arr
end
p cell_s
def cell_v
  arr = [1, 2, 3]
  o = TiP.new(-> { arr = [9] })
  r = (arr[0, 1] = o)
  [r.class, arr]
end
p cell_v
