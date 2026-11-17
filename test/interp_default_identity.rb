# spinel: gc-stress
class InterpScalar
  def initialize(value)
    @value = value
  end
  def to_s = @value
end
class InterpPlain
  def initialize(value)
    @value = value
  end
end
a = InterpScalar.new(4)
b = InterpScalar.new(4)
p "#{a}".start_with?("#<InterpScalar:")
p "#{a}" == "#{a}"
p "#{a}" == "#{b}"
c = InterpPlain.new(1)
p "#{c}" == "#{c}"
