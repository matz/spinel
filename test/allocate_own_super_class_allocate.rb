# A class's own def self.allocate that calls super gets an empty object of
# the receiving class (Class#allocate, no initialize), also through a
# subclass, a parent's own allocate and a Class value. super(1) is still
# not checked (CRuby ArgumentError), so that case is left out.
class A
  def initialize; @y = 5; puts "init"; end
  def self.allocate
    o = super()
    o.instance_variable_set(:@x, 1)
    o
  end
  attr_reader :x, :y
end
class B < A
  def self.allocate
    o = super
    o.instance_variable_set(:@z, 2)
    o
  end
  attr_reader :z
end
class C < B; end
a = A.allocate; p [a.class, a.x, a.y]
b = B.allocate; p [b.class, b.x, b.z]
c = C.allocate; p [c.class, c.x, c.z]
p A.new.y
p (begin; Class.new(A).allocate.x; rescue => e; e.class; end)
class D
  def self.allocate(n = 1)
    super()
  end
end
p D.allocate.class
class E
  def self.allocate(*)
    super
  end
end
p (E.allocate.class rescue $!.class)
module M; def allocate = super; end
class G; extend M; end
p G.allocate.class
class Er < StandardError; def self.allocate = super; end
p Er.allocate.class
S = Struct.new(:q) do
  def self.allocate = super
end
p S.allocate.class
class AS < Array; def self.allocate = super; end
p AS.allocate.class
class H
  class << self
    def allocate = super
  end
end
p H.allocate.class
k = [A, B][ARGV.size + 1]
p k.allocate.class
# allocate in a parameter default and in a yielding class method, on a subclass
class P2
  def self.make(o = allocate) = o
  def self.each_mk; yield allocate; end
end
class Q2 < P2; end
p P2.make.class, Q2.make.class
Q2.each_mk { |x| p x.class }
P2.each_mk { |x| p x.class }
