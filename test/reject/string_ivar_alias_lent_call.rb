# This route would append to a copy of the caller's String.
class D
  def add(s) = (s << "?")
  def go(x)
    @v = x
    add(@v)
  end
end
s = +"s"
D.new.go(s)
p s
