# Under --int-overflow=promote an inlined instance_exec whose block answers an
# Integer holds it as an optional Integer; assigned to a slot that boxes it
# (`r = instance_exec(x, &b) if block_given?`), it is boxed as that.

class Acc
  def initialize
    @log = []
  end
  attr_reader :log

  def with(x, &b)
    r = instance_exec(x, &b) if block_given?
    @log << r
    self
  end
end

a = Acc.new
a.with(3) { |n| with(n) { |m| m * 2 }; n }
p a.log
