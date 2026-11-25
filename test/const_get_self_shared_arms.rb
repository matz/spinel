# Run-time-name const_get on `self` in class methods: the sites of one class
# share one set of arms and one function, and each still answers its own
# name, reaches a subclass's own constant when self is the subclass, and
# names the class it runs on in the miss.
class Reg
  class Alpha; def self.v = :alpha; end
  class Beta; def self.v = :beta; end
  LIMIT = 7
  def self.find(name) = const_get(name)
  def self.pair(x, name) = [x, self.const_get(name)]
  def self.look(name) = self.const_get(name)
end

class Sub < Reg
  OWN = :sub_own
end

class Other
  class Alpha; def self.v = :other_alpha; end
  def self.find(name) = const_get(name)
end

module Tools
  WIDTH = 80
  def self.get(name) = const_get(name)
  def self.get2(name) = self.const_get(name)
end

p Reg.find("Alpha" + "").v
p Reg.pair(1, "Beta" + "")[1].v
p Reg.pair("s", "LIMIT" + "")
p Reg.pair(:k, "Alpha" + "")[1].v
p Reg.look("LIMIT" + "")
p Sub.find("OWN" + "")
p Sub.pair(2.5, "Beta" + "")[1].v
p Sub.look("LIMIT" + "")
p Other.find("Alpha" + "").v
p Tools.get("WIDTH" + "")
p Tools.get2("WIDTH" + "")

def miss
  yield
rescue NameError => e
  p e.message
end
miss { Reg.find("Nope" + "") }
miss { Sub.pair(3, "Nope" + "") }
miss { Other.find("Beta" + "") }
miss { Tools.get("Alpha" + "") }
miss { Reg.find("lower" + "") }
