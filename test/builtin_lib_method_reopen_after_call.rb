# A call the program runs at its top level ahead of a reopening's def of
# the name runs the builtin's method, also for a name builtins/integer.rb
# writes in Ruby (digits, gcd, ...) and for one the class takes from
# Comparable or Enumerable (between?, clamp, tally, min_by). After the def
# the reopening's method answers. (#10719)
p 10.digits
p 10.digits(3)
p 12.gcd(8)
p 4.lcm(6)
p 12.gcdlcm(8)
p 7.ceildiv(2)
p 7.remainder(-2)
p 7.fdiv(2)
p 255.bit_length
p 5.between?(1, 9)
p 5.clamp(1, 3)
p 2.5.between?(1, 3)
p "b".between?("a", "c")
p "z".clamp("a", "c")
p [1, 2, 3].tally
p({a: 1, b: 0}.min_by { |k, v| v })

class Integer
  def digits(b = 10) = :digits
  def gcd(o) = :gcd
  def lcm(o) = :lcm
  def gcdlcm(o) = :gcdlcm
  def ceildiv(o) = :ceildiv
  def remainder(o) = :remainder
  def fdiv(o) = :fdiv
  def bit_length = :bit_length
  def between?(a, b) = :int_between
  def clamp(a, b) = :int_clamp
end
class Float
  def between?(a, b) = :float_between
end
class String
  def between?(a, b) = :str_between
  def clamp(a, b) = :str_clamp
end
class Array
  def tally = :tally
end
class Hash
  def min_by = :min_by
end

p 10.digits
p 12.gcd(8)
p 4.lcm(6)
p 12.gcdlcm(8)
p 7.ceildiv(2)
p 7.remainder(-2)
p 7.fdiv(2)
p 255.bit_length
p 5.between?(1, 9)
p 5.clamp(1, 3)
p 2.5.between?(1, 3)
p "b".between?("a", "c")
p "z".clamp("a", "c")
p [1, 2, 3].tally
p({a: 1, b: 0}.min_by { |k, v| v })
