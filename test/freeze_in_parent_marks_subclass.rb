# A freeze in a parent class's method (here initialize) freezes a
# subclass's instance too, so the subclass's own ivar writes raise
# FrozenError, for exception and plain classes alike.
class F2 < StandardError
  def initialize; super("m"); freeze; end
end
class F3 < F2
  def setd; @d = 1; end
end
class F4 < F3
  def sete; @e = 2; end
end

class Base
  def initialize; @a = 1; freeze; end
end
class Child < Base
  def setd; @d = 1; end
end

class Plain
  def lock = self.freeze
end
class Kid < Plain
  attr_accessor :v
end

def try(label)
  yield
  puts "#{label}: no error"
rescue FrozenError => e
  puts "#{label}: #{e.class}"
end

try("exception leaf") { F3.new.setd }
try("exception grandchild") { F4.new.sete }
try("plain child") { Child.new.setd }
k = Kid.new
k.v = 1
k.lock
try("self.freeze in parent") { k.v = 2 }
try("or-write after lock") { k.v ||= 3 }
p k.v
k2 = Kid.new
k2.v = 5
p k2.v
