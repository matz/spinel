# A Class value's new with a block runs past a yielding initialize of a
# module, or of a class the receiver never is
class Box
  attr_reader :made
  def initialize(src = nil, &blk)
    @made = blk ? blk.call(src) : src
  end
  def again(pr)
    self.class.new(@made, &pr)
  end
end
class Sub < Box
end
module Hooked
  def initialize
    yield
  end
end
class Elsewhere
  def initialize
    yield 1
  end
end
Elsewhere.new { |x| p x }
pr = proc { |v| v.to_s * 2 }
p Box.new(3).again(pr).made
s = Sub.new(4).again(pr)
p s.class, s.made
