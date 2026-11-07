# method_defined? on a Struct or Data class: the members (and the
# Struct builtin methods) are defined, so a guard that reopens the class
# only when the method is absent must not run.

S1 = Struct.new(:mem)
unless S1.method_defined?(:mem, false)
  class S1; def mem = :over; end
end
p S1.new(1).mem
p S1.method_defined?(:mem, false), S1.method_defined?(:mem=, false)

S2 = Struct.new(:mem) do
  def x = 1
end
unless S2.method_defined?(:mem, false)
  class S2; def mem = :over; end
end
p S2.new(1).mem

D3 = Data.define(:mem)
unless D3.method_defined?(:mem, false)
  class D3; def mem = :over; end
end
p D3.new(1).mem
p D3.method_defined?(:mem, false), D3.method_defined?(:mem=, false)

S4 = Struct.new(:mem, keyword_init: false)
unless S4.method_defined?(:mem, false)
  class S4; def mem = :over; end
end
p S4.new(1).mem

S5 = Struct.new(:a) do
  def mem = 2
end
unless S5.method_defined?(:mem, false)
  class S5; def mem = :over; end
end
p S5.new(1).mem

S6 = Struct.new(:mem); class S6; end
unless S6.method_defined?(:mem, false)
  class S6; def mem = :over; end
end
p S6.new(1).mem

S7 = Struct.new(:mem)
unless S7.method_defined?(:mem=, false)
  class S7; def mem=(v); @x = v; end; end
end
s = S7.new(1); s.mem = 5; p s.mem

S8 = Struct.new(:a)
unless S8.method_defined?(:a, true)
  class S8; def a = :over; end
end
p S8.new(1).a
T8 = Struct.new(:b)
if T8.method_defined?(:b, false)
  class T8; def extra = 3; end
end
p T8.new(2).extra

S9 = Struct.new(:a)
unless S9.method_defined?(:each)
  class S9; def each = :over; end
end
p S9.new(1).each.class
p S9.method_defined?(:each), S9.method_defined?(:each, false), S9.method_defined?(:to_a, false)
D9 = Data.define(:a)
unless D9.method_defined?(:with)
  class D9; def with = :over; end
end
p D9.new(1).with(a: 2).a

# a private def in the block is not counted (method_defined? is false)
S10 = Struct.new(:a) do
  private
  def hid = 1
end
unless S10.method_defined?(:hid, false)
  class S10; def marker = :reopened; end
end
p S10.new(1).respond_to?(:marker)
S11 = Struct.new(:a) do
  def helper = 1
  private :helper
end
p(S11.method_defined?(:helper) ? :bad : :good)

# a record in a module is another class
module M12; S12 = Struct.new(:a); end
class S12; end
p(S12.method_defined?(:a, false) ? :bad : :good)

# a record the fold cannot read keeps the guard as before
S13 = Struct.new(:a)
class S13
  private :a
end
class S13
  def label = :s unless method_defined?(:label, false)
end
p S13.new(1).label
