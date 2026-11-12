# spinel: gc-stress
# An argument bound to a rooted temp (a String Range made in the argument list)
# that the parameter converts -- boxed into a poly parameter -- is boxed into a
# new value that nothing roots until the call: two such boxes in one argument
# list, or one beside a later argument that allocates, collected each other.
# The same arguments into a typed parameter pass the held temp unchanged.
class T
  def initialize(r, s)
    @r = r
    @s = s
  end

  def show
    "#{@r.inspect} #{@s.inspect}"
  end
end

class M
  def m(r, s) = "#{r.inspect}/#{s.inspect}"
end

class H3
  def initialize(a, b, c) = (@a = a; @b = b; @c = c)

  def show = "#{@a.inspect} #{@b.inspect} #{@c.inspect}"
end

class B
  def initialize(r, s)
    @r = r
    @s = s
  end

  def show = "#{@r.inspect} #{@s.inspect}"
end

class D < B
  def initialize(i) = super(i.to_s..(i + 1).to_s, (i + 2).to_s..(i + 3).to_s)
end

class W
  def initialize = @r = nil

  def go(i) = T.new((i + 1).to_s..(i + 2).to_s, @r = (i + 3).to_s..(i + 4).to_s)
end

T.new(1, :one)
M.new.m(1, :one)
B.new(1, :one)
H3.new(1, 2, 3)

# two Ranges into poly parameters of a constructor and of a method
res = []
mm = M.new
out = []
i = 0
while i < 120
  res << T.new(i.to_s..(i + 1).to_s, (i + 2).to_s..(i + 3).to_s)
  out << mm.m((i + 1).to_s..(i + 2).to_s, (i + 3).to_s..(i + 4).to_s)
  i += 1
end
puts res[0].show, res[119].show, res.map(&:show).uniq.size
puts out[0], out[119], out.uniq.size

# a Range beside a String that allocates, either way round
mix = []
i = 0
while i < 120
  mix << T.new(i.to_s..(i + 1).to_s, "s#{i}" + "t")
  mix << T.new("s#{i}" + "t", i.to_s..(i + 1).to_s)
  i += 1
end
puts mix[0].show, mix[1].show, mix[239].show, mix.map(&:show).uniq.size

# three arguments, the Ranges at the ends
tri = []
i = 0
while i < 120
  tri << H3.new(i.to_s..(i + 1).to_s, "m#{i}", (i + 2).to_s..(i + 3).to_s).show
  i += 1
end
puts tri[0], tri[119], tri.uniq.size

# a nested constructor call in an argument
nest = []
i = 0
while i < 120
  nest << T.new(T.new(i.to_s..(i + 1).to_s, "a#{i}" + "b").show, (i + 2).to_s..(i + 3).to_s)
  i += 1
end
puts nest[0].show, nest[119].show, nest.map(&:show).uniq.size

# super's arguments, and an assignment inside an argument
sup = []
w = W.new
asg = []
i = 0
while i < 120
  sup << D.new(i)
  asg << w.go(i)
  i += 1
end
puts sup[0].show, sup[119].show, sup.map(&:show).uniq.size
puts asg[0].show, asg[119].show, asg.map(&:show).uniq.size

# a frozen String made in the argument list is boxed into a new handle by a
# poly parameter, beside another that allocates
class F
  def initialize(a, b) = (@a = a; @b = b)

  def show = "#{@a.inspect} #{@b.inspect} #{@a.frozen?}"
end

F.new(1, :one)
frz = []
i = 0
while i < 120
  frz << F.new("a#{i}".freeze, [i, i]).show
  frz << F.new("a#{i}".freeze, "c#{i}".freeze).show
  i += 1
end
puts frz[0], frz[1], frz[239], frz.uniq.size
