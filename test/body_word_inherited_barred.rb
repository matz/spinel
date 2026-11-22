# A class method's word or non-public def acts on the class the call runs
# on, and a word naming a method the class only inherits leaves the
# inherited method's visibility in place until it runs. Where the record
# cannot place either (a class method a subclass may call, a name the
# class does not define itself, such as a module's attribute), the
# word keeps the form it has where the record holds no body versions, and
# the class it does not run on keeps its method.
class K
  def m = :m
  def self.hide = private(:m)
  def self.setup = private(def m2 = :m2)
end
class S < K; end
p [K.new.m, S.new.m]
S.hide
S.setup
p [K.new.m, K.new.respond_to?(:m), K.public_instance_methods(false).include?(:m), K.new.m2]

class A
  def a = :a
end
class B < A
  def self.hide = private(:a)
end
b = B.new
p [b.a, b.respond_to?(:a), A.new.a]
B.hide
p [A.new.a, A.private_method_defined?(:a)]

module M
  attr_reader :x
end
class C
  include M
  def initialize = (@x = 1)
  def self.hide = private(:x)
end
c = C.new
p [c.x, c.respond_to?(:x)]
C.hide
p c.respond_to?(:x, true)

# A protected method called in an instance_exec block whose receiver is an
# instance of its class is open to it there.
class P
  def self.setup = protected(def pv = 7)
end
P.setup
pr = P.new
p [P.new.instance_exec(pr) { |o| o.pv }, (pr.pv rescue $!.class), (Object.new.instance_exec(pr) { |o| o.pv } rescue $!.class)]
p [(P.class_exec(pr) { |o| o.pv } rescue $!.class), ([pr].map { |o| o.pv } rescue $!.class), P.new.instance_eval { pr.pv }]
class PL
  def run(o) = P.new.instance_exec { o.pv }
end
p [PL.new.run(pr), (P.instance_exec { pr.pv } rescue $!.class)]

# An instance_exec block on an object of no class the program defines runs
# with that object as self, to which a protected method stays closed.
class Q
  def qm = :qm
end
q = Q.new
pq = proc { class Q; protected :qm; end; nil }
p [(q.qm rescue $!.class), (Object.new.instance_exec(q) { |z| z.qm } rescue $!.class)]
pq.call
p [(q.qm rescue $!.class), (Object.new.instance_exec(q) { |z| z.qm } rescue $!.class)]
