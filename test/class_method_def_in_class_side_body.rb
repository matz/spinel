# A class method defined in the body of another class method exists from
# the first time a call runs that body, and a def of the name in a body
# that never runs is not the one the call reaches

# the defining method in a `class << K` body
class Cfg; end
class << Cfg
  def cfg=(v)
    def Cfg.k = :w
  end
end
def cfg_setup = (def Cfg.k = :mk)
p Cfg.respond_to?(:k)
Cfg.cfg = 4
p Cfg.respond_to?(:k), Cfg.k

# ...a `def self.m` in the class body
class Own
  def self.load(v)
    def Own.k = :loaded
  end
end
def own_setup = (def Own.k = :never)
p Own.respond_to?(:k)
Own.load(1)
p Own.respond_to?(:k), Own.k

# two class methods defining the name, called in turn
class Two
  def self.one = (def Two.k = :one)
  def self.two = (def Two.k = :two)
end
p(Two.k) rescue p :none
Two.two
p Two.k
Two.one
p Two.k
