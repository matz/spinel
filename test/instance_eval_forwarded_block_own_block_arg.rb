# A block given to a method that instance_evals it (Case.new { ... }) is
# spliced as definition-site code: its own `&block` names the block of the
# method it was written in (make's), not the block being spliced.
class Case
  attr_reader :got

  def initialize(&block)
    @got = []
    self.instance_eval(&block)
  end

  def is(x)
    @got << x
    x
  end

  def tag = :case
end

def make(&block)
  Case.new { is(self.instance_eval(&block)) }
end

c = make { 1 }
p c.got
d = make { tag }
p d.got
e = make { is(2) + 3 }
p e.got
