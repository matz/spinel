# A class whose to_ary answers one tuple has a subclass whose own to_ary may
# answer nil: a value typed as the class is converted by the method that runs.
# The same for String and a String subclass that defines its own to_ary.
class A
  def to_ary = [self, 1]
  def inspect = self.class.name
  def go = (a, b = self; [a, b])
end
class B < A
  def to_ary
    return nil if $off
    [self, 2]
  end
end
$off = true
p A.new.go
p B.new.go
$off = false
p B.new.go

class String
  def to_ary = [self, 1]
  def go = (a, b = self; [a, b])
end
class Sub < String
  def to_ary = nil
end
p (+"x").go
p Sub.new("y").go

# a private or protected to_ary of the subclass is called by the implicit
# conversion as well
class String
  def to_ary = [self, 1]
  def go2 = (a, b = self; [a, b])
end
class PrivSub < String
  private def to_ary = nil
end
class ProtSub < String
  protected def to_ary = nil
end
p PrivSub.new("p").go2
p ProtSub.new("q").go2
p (+"x").go2
def yl(v) = yield(v)
yl([PrivSub.new("r"), 1].first) { |a, b| p [a, b] }
yl(ProtSub.new("s")) { |a, b| p [a, b] }
