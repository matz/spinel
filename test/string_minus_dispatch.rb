# String's own operator, reflected calls, and subclass overrides keep dispatch.
class String
  def -(other)
    self + other.to_s
  end
end
puts "a" - 1
copy = "b"
puts copy - 2
puts "c".send(:-, 3)
puts "d".public_send(:-, 4)
def subtract(value)
  value - 5
end
puts subtract("e")
class Label < String
  def -(other)
    "own:" + other.to_s
  end
end
class InheritedLabel < String
end
puts Label.new("x") - 6
puts InheritedLabel.new("y") - 7
