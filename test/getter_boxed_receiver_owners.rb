# A runtime-broad receiver can write through either owner's getter.
class Left
  def initialize = @letters = {"s" => "x"}
  def data = @letters
end
class Right
  def initialize = @numbers = {1 => 2}
  def data = @numbers
end
def put(obj, key, value)
  obj.data[key] = value
end
a = Left.new
b = Right.new
[a, b].each { |obj| put(obj, :added, nil) }
put(a, 7, "seven")
put(b, "word", "value")
p a.data
p b.data
