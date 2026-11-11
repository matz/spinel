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
p "s".dual6

# the program's own Complex or Rational method answers first
module Comparable; def lbl6 = "cmp"; end
class Complex; def lbl6 = "cpx own"; end
class Rational; def lbl6 = "rat own"; end
p Complex(1, 2).lbl6, 3r.lbl6, "s".lbl6
# a name only Numeric has keeps the Comparable method for a String
module Comparable; def truncate = "cmp truncate"; def wt6 = truncate; end
p "s".wt6, :s.truncate

# a written `is_a?(Complex)` arm in an Object method is not a Complex method
class Object
  def probe6
    if is_a?(Complex)
      :object
    end
  end
end
module Comparable; def probe6 = :comparable; end
p Complex(1, 2).probe6

# a Complex method called from a Comparable method answers for the Complex
module Comparable; def dual7 = "cmp"; def both7 = [dual7, tag6(1)]; end
class Complex; def dual7 = "cpx"; end
p Complex(1, 2).both7
