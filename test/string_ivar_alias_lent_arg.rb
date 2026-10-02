# A fresh ivar and an ordinary local can still lend their own String slot.
class D
  def add(s) = (s << "?")
  def go
    t = +"t"
    add(t)
    p t
    @v = +"own"
    add(@v)
    p @v
  end
end
D.new.go
