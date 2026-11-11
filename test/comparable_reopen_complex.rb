# A method the program adds to `module Comparable` answers for a Complex
# (Complex < Numeric, which includes Comparable), also when Object has a
# method of the same name. A builtin Complex or Numeric method of the name
# answers first.
module Comparable
  def dual6 = "cmp"
  def tag6(x) = "t#{x}"
  def both6 = [dual6, tag6(1)]
  def integer? = "cmp integer?"
  def real = "cmp real"
end
class Object; def dual6 = "obj"; end
z = Complex(1, 2)
p z.dual6, z.both6, Complex(3, 0).tag6(2)
p z.integer?, z.real
def via(c) = c.dual6
p via(Complex(0, 1))
p 3r.dual6, "s".dual6
