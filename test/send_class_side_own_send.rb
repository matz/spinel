# A class's own `def self.send` (or `public_send`, or one a `class << self` or
# `singleton_class.define_method` gives it) answers a
# send made on the class, as an instance's does: the class's singleton method
# is reached by a send call and run, whether the receiver is the constant or a
# value that may hold the class (a boxed one), and the sends the class does
# not answer stay Object#send.

class K
  def self.send(*a) = [:ksend, *a]
  def self.m = :km
end
p K.send(:m)
p K.__send__(:m)

class Sc
  class << self
    def send(*a) = [:scsend, *a]
    def m = :scm
  end
end
class Plain
  def m = :pm
end
x = [Sc, Plain.new][ARGV.size]
p x.send(:m)

class Dm
  def self.m = :dm
end
Dm.singleton_class.define_method(:send) { |*a| [:dmsend, *a] }
p Dm.send(:m)
y = [Dm, Plain.new][ARGV.size]
p y.send(:m)

class Ps
  def self.public_send(*a) = [:psend, *a]
  def self.m = :psm
end
p Ps.public_send(:m)
p Ps.send(:m)
z = [Ps, Plain.new][ARGV.size]
p z.public_send(:m)

module U
  def self.send(*a) = [:usend, *a]
  def self.m = :um
end
p U.send(:m)
w = [U, 3][ARGV.size]
p w.send(:+, 1)

# a class with no send of its own: Object#send runs the named method
class Q
  def self.m = :qm
end
p Q.send(:m)
v = [Q, Plain.new][ARGV.size]
p v.send(:m)
