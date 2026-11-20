# A module whose method a method body defines later (`def self.setup =
# (def j = ...)`) has no method in the lookup of a class that mixes it in
# until that body runs: a call finds the method behind the module's copy,
# the class's own def, or the superclass's, as CRuby's lookup passes the
# module. Before, a call found the copy. Each shape below puts the module
# in a different place of the class's chain, and calls it with arguments,
# a block, a nil receiver, a boxed one and through send, before and after.

# a reopening's prepend over the class's own def, one argument form each
module Q1; def self.setup = (def j(a, b = 2, &blk) = [:q1, a, b, blk&.call, super(a)]); end
class K1; def j(a, b = 2) = [:k1, a, b]; end
class K1; prepend Q1; end
def show1(tag, k) = p([tag, k.j(1), k.j(1, 3) { :blk }, [k, nil].map { |x| x&.j(7) }, [k, 1].map { |x| x.respond_to?(:j) ? x.j(4) : :no }, k.send(:j, 5), k.public_send(:j, 6)])
show1(:before, K1.new)
Q1.setup
show1(:after, K1.new)

# the prepend is a call's statement, made after calls
module Q2; def self.setup = (def j = [:q2, super]); end
class K2; def j = :k2; end
k2 = K2.new
p [k2.j]
K2.prepend(Q2)
p [k2.j]
Q2.setup
p [k2.j, K2.new.j]

# in the class body, in front of the own def written below it, and a subclass
module Q3; def self.setup = (def j = [:q3, super]); end
class K3
  prepend Q3
  def j = :k3
end
class S3 < K3; end
p [K3.new.j, S3.new.j, [K3.new, S3.new].map(&:j)]
Q3.setup
p [K3.new.j, S3.new.j, [K3.new, S3.new].map(&:j)]

# two modules late, one in the body, one in a reopening: each tested
module Q4; def self.setup = (def j = [:q4]); end
module Q5; def self.setup = (def j = [:q5]); end
class K4
  prepend Q4
  def j = :k4
end
class K4; prepend Q5; end
k4 = K4.new
p [k4.j]
Q4.setup
p [k4.j]
Q5.setup
p [k4.j]

# an include's copy over the superclass's method, the class having none
module Q6; def self.setup = (def j = [:q6, super]); end
class P6; def j = :p6; end
class K6 < P6; end
class K6; include Q6; end
k6 = K6.new
p [k6.j, K6.new.j]
Q6.setup
p [k6.j]

# the class side
module Q7; def self.setup = (def j = [:q7]); end
class K7; def self.j = :k7; end
class << K7; prepend Q7; end
p [K7.j]
Q7.setup
p [K7.j]

# a late layer in front of a class whose superclass's method a later prepend
# guards: the lookup behind the layer finds that prepend's arms first, as a
# prepend that did not run is passed, and one that did is found
module Q8; def self.setup = (def jb = [:q8, super]); end
module P8; def jb = [:p8, super]; end
class B8; def jb = [:b8]; end
class K8 < B8; prepend Q8; end
p [K8.new.jb]
class B8; prepend P8; end
p [K8.new.jb]
Q8.setup
p [K8.new.jb]
