# A module_function method copied into an includer must call the instance
# copy of a sibling, not a singleton method the program defines later.

module U1
  module_function
  def a(x) = x + 1
  def b(x) = a(x) * 2
end
class H1; include U1; def t = b(3); end
def U1.a(x) = 100
p H1.new.t
p U1.b(3)

module U2
  module_function
  def a(x) = x + 1
  def b(x) = a(x) * 2
  class << self
    def a(x) = 50
  end
end
class H2; include U2; def t = b(3); end
p H2.new.t
p U2.b(3)
p H2.new.send(:a, 1)

module U3
  def a(x) = x + 1
  module_function :a
  def b(x) = a(x) * 2
  module_function :b
end
module U3
  def self.a(x) = 100
end
class H3; include U3; def t = b(3); end
p H3.new.t
p U3.b(3)

module U4
  module_function
  def a(x) = x + 1
  def c(x) = a(x) + 10
  def b(x) = c(x) * 2
end
def U4.a(x) = 100
class H4; include U4; def t = b(3); end
class G4; include U4; def t = a(3); end
p H4.new.t, G4.new.t
p U4.b(3), U4.c(1)

module U5
  module_function
  def a(x) = x + 1
  def b(x) = a(x) * 2
end
def U5.b(x) = -1
class H5; include U5; def t = b(3); end
p H5.new.t
p U5.b(3), U5.a(0)
