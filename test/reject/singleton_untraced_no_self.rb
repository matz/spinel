# `def <recv>.m` on a receiver spinel cannot trace to one `new` of a user
# class or of Object has no synthesized subclass to carry the method. One
# whose body needs no self compiled as a plain function that nothing calls,
# so the program ran on as if the def were not there: for each one below
# CRuby prints the singleton method's answer, and the compiled program
# printed what the object answers without it. Each is refused at its def.

# a String (minitest's long_invisible assertion test)
def string_eq
  exp = "blah" * 10
  def exp.== _
    false
  end
  p exp == "blah" * 10
end

# an object read back out of a Hash
def hash_value
  h = {1 => Object.new}
  o = h[1]
  def o.inspect = "X"
  p h[1].inspect
end

# an Array
def array_size
  a = [1, 2]
  def a.size = 99
  p a.size
end

# a constant set to a String
NAME = "abc" * 1
def const_string
  def NAME.upcase = "zzz"
  p NAME.upcase
end

# a builtin's object constant
def env_fetch
  def ENV.fetch(_) = "stub"
  p ENV.fetch("HOME")
end

# a method parameter
def tag(s)
  def s.to_s = "tagged"
  s
end

# an expression
def expression
  h = {1 => Object.new}
  def (h.fetch(1)).to_s = "T"
  puts h[1].to_s
end

# a class built by Struct.new, held in a local
def struct_class
  struct = Struct.new(:a)
  def struct.name = "A"
  p struct.name
end

# a constant set in a class body
class Holder
  K = "abc" * 1
  def K.upcase = "zzz"
  p K.upcase
end

# a builtin's object constant in parentheses
def paren_argv
  def (ARGV).first = :stub
  p ARGV.first
end

# a constant set by ||=
def or_write_const
  def OR_NAME.upcase = "zzz"
  p OR_NAME.upcase
end
OR_NAME ||= "abc" * 1

# `def self.m` inside an instance method of a reopened String is the one
# String's
class String
  def tag!
    def self.size = 99
    self
  end
end

string_eq
hash_value
array_size
const_string
env_fetch
puts tag(+"x").to_s
expression
struct_class
or_write_const
paren_argv
p (+"abc").tag!.size
