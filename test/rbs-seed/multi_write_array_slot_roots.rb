class TupleArraySlot
  def save(target, value)
    *, target, first = value, +"first"
    (@array, ignored), second = [value, 1], +"second"
    first << "!"
    second << "!"
    p target, @array, ignored, first, second
  end
end

def tuple_values(flag)
  flag ? [41, 42] : :other
end

TupleArraySlot.new.save([1, "marker"], tuple_values(true))
