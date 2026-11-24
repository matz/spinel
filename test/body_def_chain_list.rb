# A list of a chain's methods (`x.methods`, `x.public_methods`, `K.instance_methods`,
# `K.public_instance_methods`) reads the version cells of a method a class method's
# body defines, as a call of it there finds it: the name is listed once the body has
# run, not before, and the list agrees with the guard that reads it, whether the
# receiver is one class, a boxed value of several, or the guard a method's tail.
class K
  def self.setup
    def m = :k
    def n = :kn
  end
  def o = :o
  private def pv = :pv
end
class J < K
  def j = :j
end
class P
  def m = :p
end

# one class: the lists before and after the body runs, and the call
def one(tag, x)
  p [tag, x.methods.include?(:m), x.public_methods.include?(:m), x.methods.include?(:o),
     x.methods.include?(:pv), (x.m rescue $!.class)]
end
one(:k0, K.new)
one(:j0, J.new)
p [K.instance_methods.include?(:m), K.public_instance_methods.include?(:n), J.instance_methods.include?(:m), K.instance_methods.include?(:o)]
K.setup
one(:k1, K.new)
one(:j1, J.new)
p [K.instance_methods.include?(:m), K.public_instance_methods.include?(:n), J.instance_methods.include?(:m), K.instance_methods.include?(:o)]

# a boxed receiver of several classes, one of which defines the name itself
class Q
  def self.setup = (def q = :q)
end
class R < Q; end
class S
  def q = :s
end
xs = [Q.new, R.new, S.new]
guard = proc { xs.map { |x| x.methods.include?(:q) ? x.q : :none } }
pguard = proc { xs.map { |x| x.public_methods.include?(:q) ? x.q : :none } }
p guard.call, pguard.call
Q.setup
p guard.call, pguard.call

# the guard in tail position of a method, called before and after
class T
  def self.setup = (def t = :t)
end
def tail_guard = T.new.methods.include?(:t) ? T.new.t : :none
def tail_public = T.new.public_methods.include?(:t) ? T.new.t : :none
p [tail_guard, tail_public]
T.setup
p [tail_guard, tail_public]

# a body that never runs lists nothing, and an unrelated list does not change that
class N
  def self.setup = (def never = :never)
end
p [N.new.methods.include?(:never), (N.new.never rescue $!.class), 1.methods.include?(:never), "s".public_methods.include?(:never)]
