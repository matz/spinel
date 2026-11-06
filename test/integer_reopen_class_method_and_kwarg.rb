# `def self.times` in `class Integer` is a method of the class, not of 3.
class Integer
  def self.times(*a) = "cls"
  def self.step(*a, &b) = "clsstep"
end
p 3.times { }
p 1.step(3, 1) { }
p Integer.times
# a later reopen with a keyword parameter replaces an earlier one
class Integer
  def foo(a) = "f1"
end
class Integer
  def foo(*a, k: 2) = "f2#{k}"
end
p 1.foo(2)
# a `*arr` argument fills the required parameters
class Integer
  def upto(a, b, *r) = [a, b, r]
end
arr = [3, 1]
p 1.upto(*arr) { }
p 1.upto(*arr)
more = [3, 1, 9]
p 1.upto(*more) { }
