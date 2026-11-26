# a method of the program's named attr or attr_reader, called with a value
# that is not a literal: the program's method runs
class Box
  def attr(x) = "attr #{x}"
  def show(v) = attr(v)
end

module Reader
  def attr_reader(name) = "reader #{name}"
end

class Doc
  include Reader
  def go(n) = attr_reader(n)
end

p Box.new.show(3), Doc.new.go(:x)
