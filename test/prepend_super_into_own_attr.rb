# `super` from a prepended module's method reaches the class's own
# attr_reader, attr_writer or Struct member when the class has no def of
# the name: it reads or writes the ivar
module M
  def name = "M:" + super.to_s
end
module W
  def age=(v)
    super(v * 2)
  end
end
module M2
  def name = "M2:" + super
end
module M3
  def name = "M3:" + super()
end

class A1; attr_reader :name; def initialize = @name = "a1"; prepend M; end
p A1.new.name
class A2; prepend M; attr_reader :name; def initialize = @name = "a2"; end
p A2.new.name
class A3; attr_reader :name; def initialize = @name = "a3"; def name = "own3"; prepend M; end
p A3.new.name
class A5; attr_accessor :age; prepend W; end
x = A5.new
x.age = 4
p x.age
class A7; attr_reader :name; def initialize = @name = "a7"; end
A7.prepend(M)
p A7.new.name
class A8; attr_reader :name; def initialize = @name = "a8"; prepend M; prepend M2; end
p A8.new.name
class G10; attr_reader :name; def initialize = @name = "g10"; end
class P10 < G10; def name = "p10"; end
class A10 < P10; prepend M; end
p A10.new.name
class A11 < P10; attr_reader :name; prepend M; end
p A11.new.name
class A13; attr_reader :name; prepend M; end
p A13.new.name
class A16; attr_reader :name; def initialize = @name = "a16"; prepend M3; end
p A16.new.name

S1 = Struct.new(:name)
S1.prepend(M)
p S1.new("s1").name
D1 = Data.define(:name) do
  prepend M
end
p D1.new(name: "d1").name
S4 = Struct.new(:name) do
  def name = "own4"
  prepend M
end
p S4.new("x").name
S5 = Struct.new(:name, keyword_init: true)
S5.prepend(M)
p S5.new(name: "k").name
module WN
  def name=(v)
    super(v * 3)
  end
end
S6 = Struct.new(:name) { prepend WN }
s6 = S6.new(1)
s6.name = 5
p s6.name
