# A class's own `send` (or `__send__`, `public_send`) whose whole body is a
# `super(...)` that hands its parameters on as they came -- `*a`, `...`, a
# name, a splat and a block -- answers what Object#send does, as a bare `super`
# does: it is no send of the program's own, so a send on the class runs the
# named method, however the def is written.

class K1
  def self.send(*a) = super(*a)
  def self.m = :k1
end
p K1.send(:m)

class K2
  def self.send(...) = super(...)
  def self.m = :k2
end
p K2.send(:m)

class K3
  def self.send(name, *args, &b) = super(name, *args, &b)
  def self.m(x) = [:k3, x]
end
p K3.send(:m, 1)

class K4
  class << self
    def send(*a) = super(*a)
    def m = :k4
  end
end
p K4.send(:m)

class K5
  def self.public_send(name, *args) = super(name, *args)
  def self.m(x, y) = x + y
end
p K5.public_send(:m, 2, 3)

class K6
  def self.__send__(*a, &b) = super(*a, &b)
  def self.m = yield
end
p K6.__send__(:m) { :k6 }

# on a value that may hold the class
class Plain
  def m = :plain
end
v = [K1, Plain.new][ARGV.size]
p v.send(:m)
w = [Plain.new, K2][ARGV.size]
p w.send(:m)

# keywords passed on whole, as `**kw` (or the anonymous `**`), with the others
class K7
  def self.send(name, **kw) = super(name, **kw)
  def self.m(**kw) = [:k7, kw]
end
p K7.send(:m)
p K7.send(:m, a: 1)

class K8
  def self.send(name, *a, **kw, &b) = super(name, *a, **kw, &b)
  def self.m(*a, **kw) = [:k8, a, kw, block_given?]
end
p K8.send(:m)
p K8.send(:m, 1, a: 2) { }

class K9
  def self.send(name, *, **, &) = super(name, *, **, &)
  def self.m(*a, **kw) = [:k9, a, kw]
end
p K9.send(:m, 1, a: 2)

# `**nil` takes no keyword and hands none on
class K10
  def self.send(name, **nil) = super(name)
  def self.m = :k10
end
p K10.send(:m)
class K11
  def self.send(name, *a, **nil, &b) = super(name, *a, &b)
  def self.m(*a) = [:k11, a, block_given?]
end
p K11.send(:m, 1) { }
