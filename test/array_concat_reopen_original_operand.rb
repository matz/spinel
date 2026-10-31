$log = []
class ConcatOriginalOperand
  def to_ary
    $log << :conversion
    [9]
  end
end
class Array
  def concat(x)
    $log << x.class
    self
  end
end
a = [1]
x = ConcatOriginalOperand.new
a.concat(x)
p a.concat(x).equal?(a)
p a
p $log
