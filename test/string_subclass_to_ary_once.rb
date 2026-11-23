# A String subclass that inherits the program's String#to_ary has it called
# once where a block or an assignment takes an instance apart, also when it
# answers nil (the instance is kept); one with its own to_ary calls only that.
$calls = []
class String
  def to_ary
    $calls << self.class
    return nil if self == "nil"
    [self, :str]
  end
end
class A < String; end
class B < String
  def to_ary
    $calls << [:b, self.class]
    nil
  end
end
def yl(v) = yield(v)

yl([A.new("nil"), 1].first) { |x, y| p [x, y] }
x, y = [A.new("nil"), 1].first
p [x, y]
yl(A.new("nil")) { |x, y| p [x, y] }
yl([A.new("ok"), 1].first) { |x, y| p [x, y] }
yl([B.new("b"), 1].first) { |x, y| p [x, y] }
x, y = B.new("b")
p [x, y]
yl("nil") { |x, y| p [x, y] }
p $calls
