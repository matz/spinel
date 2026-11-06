# An include a reopening runs after the class's first body, of a module a
# later reopening redefines a method of: until the include has run a call
# reaches what the class answered, after it the version the module holds
# when the call runs. The guard entry tells the two apart by the include's
# cell and the module's version cell.
module V1
  def a = 1
end
class P1; def a = :p; end
class K1 < P1; end
p K1.new.a
class K1; include V1; end
p K1.new.a
module V1; def a = [super, 2]; end
p K1.new.a

# a call in another of the module's methods, whose copy runs after both
module V2
  def a = 1
  def b = a * 10
end
class K2; end
k = K2.new
p((k.b rescue :nme))
class K2; include V2; end
p k.b
module V2; def a = 2; end
p k.b

# Methods made at each point
module V3
  def a = 1
end
class P3; def a = :p; end
class K3 < P3; end
k3 = K3.new
m0 = k3.method(:a)
class K3; include V3; end
m1 = k3.method(:a)
module V3; def a = [super, 2]; end
m2 = k3.method(:a)
p m0.call, m1.call, m2.call

# versions of different parameters, through a method that calls the name
module V4
  def a(x) = x
  def a(x, y = 2) = [super(x), y]
end
class P4; def a(x) = [:p, x]; end
class K4 < P4; end
def run4(k) = k.a(1)
k4 = K4.new
p run4(k4)
class K4; include V4; end
p run4(k4)
