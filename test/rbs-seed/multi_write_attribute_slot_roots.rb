class TupleAttributeSlotBase
  attr_accessor :array, :number
end

class TupleAttributeSlot < TupleAttributeSlotBase
  def assign(value)
    self.array, left = value, +"left"
    *, self.array, right = value, +"right"
    (self.array, ignored), nested = [value, 1], +"nested"
    self.number, scalar = 17, +"scalar"
    left << "!"
    right << "!"
    nested << "!"
    scalar << "!"
    p [array, number, ignored, left, right, nested, scalar]
  end
end

def tuple_attribute_value(flag)
  flag ? [41, 42] : :other
end

TupleAttributeSlot.new.assign(tuple_attribute_value(true))
