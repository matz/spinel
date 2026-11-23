# A builtin class's own class method that builds the class by a bare
# `new` (`class String; def self.blank = new(...)`): the C called the
# program-class constructor with no initialize to take the argument and did
# not build. It is `self.new`, as in Ruby.
class String
  def self.blank = String.new("ab")
end
p String.blank
class Array
  def self.pair(x) = new(2, x)
end
p Array.pair(:z)
