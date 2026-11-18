# A conversion can move the receiver's bytes before the comparison reads them.
class GrowingCasecmpOperand
  def initialize(receiver)
    @receiver = receiver
  end

  def to_str
    @receiver << "z" * 1200
    GC.start
    @receiver
  end
end

def compare(receiver, operand)
  p receiver.casecmp(operand)
  p receiver.casecmp?(operand)
end

typed = +"a"
boxed = +"a"
receivers = [boxed, :symbol]
operands = [GrowingCasecmpOperand.new(typed), GrowingCasecmpOperand.new(boxed), nil]
p typed.casecmp(operands[0])
p typed.casecmp?(GrowingCasecmpOperand.new(typed))
compare(receivers[0], operands[1])
