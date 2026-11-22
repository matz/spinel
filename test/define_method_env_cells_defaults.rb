# A block parameter's default is the method's own code, in the method's environment, whatever it hoists to run:
# an Array, a Hash or a block of its own that reads a captured local, a lambda that closes over one, on an
# instance method and a singleton method, and a method that takes a block (called with a literal block, with a
# Proc and with `&` of a call that runs the definer) reads its captured local through its environment too.
class Dflt
  def self.mk(n)
    define_method(:arr) { |a, b = [n]| [a, b] }
    define_method(:hsh) { |a, b = {k: n}| [a, b] }
    define_method(:mapped) { |a, b = [n].map { |q| q + 1 }| [a, b] }
    define_method(:lam) { |a, b = -> { n }| [a, b.call] }
    define_singleton_method(:sing) { |a = [n]| a }
    :ok
  end
end
begin
  Dflt.sing
rescue NoMethodError => e
  puts e.message
end
Dflt.mk(3)
d = Dflt.new
p d.arr(0), d.arr(0, 1), d.hsh(0), d.mapped(0), d.lam(0), Dflt.sing, Dflt.sing(1)
class Blk
  def self.mk(n)
    define_method(:m) { |&b| [n, b.call] }
    define_method(:w) { |a = n, &b| [a, b.call] }
    :ok
  end
  def self.fresh
    mk(7)
    proc { :made }
  end
end
Blk.mk(5)
b = Blk.new
pr = proc { :pr }
p b.m { :lit }, b.m(&pr), b.w { :lit }, b.w(1, &pr)
p b.m(&Blk.fresh), b.m { :again }
# a proc that the defined method makes closes over the captured local too
class Inner
  def self.mk(n)
    define_method(:make) { |q| [-> { n + 1 }, proc { |z| n * z }] }
    :ok
  end
end
Inner.mk(4)
a, c = Inner.new.make(1)
p a.call, c.call(3)
Inner.mk(5)
p a.call, Inner.new.make(2)[0].call
