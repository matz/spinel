# This route would append to a copy of the caller's String.
class B
  def initialize(s) = (s << "!")
end
class C < B
  def initialize(x)
    @v = x
    super(@v)
  end
end
s = +"s"
C.new(s)
p s
