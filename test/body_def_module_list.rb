# A list of a module's instance methods (`M.instance_methods`, `M.public_instance_methods`
# and their `(false)` forms) reads the version cells of a method a module's body defines,
# as the list of a class does: the name is listed once the body has run, and a guard
# over the list agrees with the call it guards, in a lambda, in tail position, and
# through a class that includes or prepends the module.
module M
  def placed = :placed
  def self.setup = (def mm = :m)
end
class K
  include M
end
def show = [M.instance_methods.include?(:mm), M.public_instance_methods.include?(:mm), M.instance_methods(true).include?(:mm),
            M.instance_methods(false).include?(:mm), M.public_instance_methods(false).include?(:mm),
            M.instance_methods.include?(:placed), K.instance_methods.include?(:mm), K.new.methods.include?(:mm),
            (K.new.mm rescue $!.class)]
p show
M.setup
p show

# the guard in tail position and in a lambda, before and after the body
module N
  def self.setup = (def nn = :n)
end
class L
  include N
end
def pick = N.instance_methods.include?(:nn) ? L.new.nn : :none
def pick_own = N.public_instance_methods(false).include?(:nn) ? L.new.nn : :none
chk = -> { N.instance_methods.include?(:nn) }
p [pick, pick_own, chk.call]
N.setup
p [pick, pick_own, chk.call]

# a prepended module, and two modules that define one name
module P
  def self.setup = (def pq = :p)
end
class Q
  prepend P
end
module A
  def self.setup = (def aa = :a)
end
module B
  def self.setup = (def aa = :b)
end
class R
  include A
  include B
end
def other = [P.instance_methods.include?(:pq), Q.instance_methods.include?(:pq), Q.new.methods.include?(:pq),
             A.instance_methods.include?(:aa), B.instance_methods.include?(:aa), R.instance_methods.include?(:aa), (R.new.aa rescue $!.class), (Q.new.pq rescue $!.class)]
p other
P.setup
A.setup
p other
B.setup
p other
