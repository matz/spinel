class C
  def initialize; @x = 9; end
  def go(&b) = instance_eval(&b)
  def run; go { @x + 1 }; end
end
p C.new.run
class C
  def go(&b) = b.call
  def run2; go { 42 }; end
end
p C.new.run2
