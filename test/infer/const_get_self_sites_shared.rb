# Three run-time-name const_get calls on `self` in class methods of one class
# share one function of the arms (#12511), which takes the class self is
# for the miss, not a copy of the arms at each site.
class Reg
  class K1; def self.v = 1; end
  class K2; def self.v = 2; end
  class K3; def self.v = 3; end
  class K4; def self.v = 4; end
  class K5; def self.v = 5; end
  class K6; def self.v = 6; end
  class K7; def self.v = 7; end
  class K8; def self.v = 8; end
  class K9; def self.v = 9; end
  class K10; def self.v = 10; end
  class K11; def self.v = 11; end
  class K12; def self.v = 12; end
  def self.a(n) = const_get(n)
  def self.b(x, n) = [x, self.const_get(n)]
  def self.c(n) = self.const_get(n).v
end
class Sub < Reg; end
p Reg.a("K1" + "").v
p Reg.b(1, "K2" + "")[1].v
p Reg.c("K7" + "")
p Sub.c("K3" + "")
begin; Sub.a("K99" + ""); rescue NameError => e; p e.message; end
