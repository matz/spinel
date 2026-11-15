# A call through a receiver of more than one class reaches a method its
# class inherits from a parent that redefines it between the calls.

class Base
  def setup = "base1"
end
class A < Base; end
class B; def setup = "b"; end

def run(t) = t.setup

p run(A.new)
class Base
  def setup = "base2"
end
p run(A.new)
p run(B.new)

# two levels down, beside a class that keeps the root's method
class Root
  def prepare = "root"
end
class Mid < Root
  def prepare = "mid1"
end
class Leaf < Mid; end
class Other < Root; end

def prep(t) = t.prepare

p prep(Leaf.new)
class Mid < Root
  def prepare = "mid2"
end
class Late < Mid; end
p prep(Leaf.new)
p prep(Other.new)
p prep(Late.new)

# a class that owns the method beside one that inherits it
class Own
  def go = "own1"
end
class Heir < Own; end
class Stranger; def go = "stranger"; end

def go(t) = t.go

p go(Own.new)
p go(Heir.new)
class Own
  def go = "own2"
end
p go(Own.new)
p go(Heir.new)
p go(Stranger.new)

# a receiver that lacks the method still raises
class Plain; end
def poke(t) = t.setup
begin
  [A.new, Plain.new].each { |t| p poke(t) }
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end
