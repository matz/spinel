# A call of a method that a class object answers with another arity (Module#name takes no
# argument) is checked against that arity unless the program defines the method on the
# constant's singleton. Each constant in the first group defines `name` (or `members`, for a
# Struct or Data class) with one parameter in one of the ways a program can, so the call is
# right; each in the second does not define it on the singleton, so the call is an
# ArgumentError, as in CRuby. The class's own method table answers these calls as well as
# singleton_def_of's list of the declarations does, so the test pins the answers whichever
# of the two gives them. It does not cover a module whose `extend self` is under a
# conditional, a loop, an ensure or a block: there the list alone decides, and the answer
# is not CRuby's.
class S1; end
def S1.name(x) = "s1:#{x}"

class S2; end
class << S2
  def name(x) = "s2:#{x}"
end

class S3
  class << self
    def name(x) = "s3:#{x}"
  end
end

class S4
  class << self
    define_method(:name) { |x| "s4:#{x}" }
  end
end

class S5
  [1].each do
    class << self
      def name(x) = "s5:#{x}"
    end
  end
end

class S6
  class << self
    if true
      def name(x) = "s6:#{x}"
    end
  end
end

module M7
  extend self
  def name(x) = "m7:#{x}"
end

class S8
  class << self
    [1].each do
      def name(x) = "s8:#{x}"
    end
  end
end

class S9
  class << self
    begin
      def name(x) = "s9:#{x}"
    rescue StandardError
      nil
    end
  end
end

# a class Struct.new or Data.define answers is not yet a class when the defs are read, so only
# the declarations tell: a def on the constant, a `class <<` of it, a reopening's singleton body,
# and a def under a conditional in one
P1 = Struct.new(:a)
def P1.members(x) = "p1:#{x}"

P2 = Struct.new(:a)
class << P2
  def members(x) = "p2:#{x}"
end

P3 = Data.define(:a)
class P3
  class << self
    def members(x) = "p3:#{x}"
  end
end

P4 = Struct.new(:a)
class << P4
  if true
    def members(x) = "p4:#{x}"
  end
end

puts S1.name(1), S2.name(2), S3.name(3), S4.name(4), S5.name(5), S6.name(6), M7.name(7), S8.name(8), S9.name(9)
puts P1.members(1), P2.members(2), P3.members(3), P4.members(4)

# not defined on the constant's singleton
class T1; end
class T2; end
def T2.other = 1
class T3; end
def T3b = 1
class T5
  def name(x) = "t5:#{x}"
end
module T6
  def name(x) = "t6:#{x}"
end
# (the control for the rest: defined, so no error)
class T7; end
def T7.name(x) = "t7:#{x}"
class T8; end
P5 = Struct.new(:a)
P6 = Struct.new(:a)
def P6.other = 1

begin; T1.name(1); puts "T1 no error"; rescue ArgumentError; puts "T1 ArgumentError"; end
begin; T2.name(1); puts "T2 no error"; rescue ArgumentError; puts "T2 ArgumentError"; end
begin; T3.name(1); puts "T3 no error"; rescue ArgumentError; puts "T3 ArgumentError"; end
begin; T5.name(1); puts "T5 no error"; rescue ArgumentError; puts "T5 ArgumentError"; end
begin; T6.name(1); puts "T6 no error"; rescue ArgumentError; puts "T6 ArgumentError"; end
begin; P5.members(1); puts "P5 no error"; rescue ArgumentError; puts "P5 ArgumentError"; end
begin; P6.members(1); puts "P6 no error"; rescue ArgumentError; puts "P6 ArgumentError"; end
begin; T8.name(1); puts "T8 no error"; rescue ArgumentError; puts "T8 ArgumentError"; end
puts T7.name(7)
