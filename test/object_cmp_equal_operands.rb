# Object#<=> answers 0 for an equal operand as well as for the same object:
# a Struct's or a Data's field-wise ==, or a class's own ==. The same object
# is 0 without asking ==.
S = Struct.new(:a)
s = S.new(1)
p S.new(1) <=> S.new(1)
p S.new(1) <=> S.new(2)
p s <=> s

D = Data.define(:a)
p D.new(1) <=> D.new(1)
p D.new(1) <=> D.new(2)

class U
  attr_reader :v
  def initialize(v) = @v = v
  def ==(o)
    puts "== asked"
    o.is_a?(U) && v == o.v
  end
end
u = U.new(1)
p U.new(1) <=> U.new(1)
p U.new(1) <=> U.new(2)
p u <=> u

class Any
  def ==(o) = true
end
p Any.new <=> 5

class Plain; end
pl = Plain.new
p pl <=> pl
p Plain.new <=> Plain.new
