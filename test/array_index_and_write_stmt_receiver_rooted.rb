# spinel: gc-minor
# spinel: gc-stress
# A statement `a[i] &&= v` / `a[i] ||= v` keeps its Array alive while the index runs, when it
# drops the last other reference to the Array.
def churn(n)
  k = 0
  n.times { |i| k += ("y" * 50 + i.to_s).size }
  k
end

class Box
  attr_accessor :i, :f, :in_, :fn

  def initialize
    @i = [1, 2, 3]
    @f = [1.5, 2.5, 3.5]
    @in_ = [1, nil, 3]
    @fn = [1.5, nil, 3.5]
  end

  def swap!
    @i = [9]
    @f = [9.5]
    @in_ = [9]
    @fn = [9.5]
    churn(3000)
  end

  def show
    puts [@i, @f, @in_, @fn].inspect
  end
end

bx = Box.new
bx.i[(bx.swap!; 1)] &&= 6
bx.show

bx = Box.new
bx.f[(bx.swap!; 1)] &&= 6.5
bx.show

bx = Box.new
bx.in_[(bx.swap!; 1)] ||= 6
bx.show

bx = Box.new
bx.fn[(bx.swap!; 1)] ||= 6.5
bx.show
