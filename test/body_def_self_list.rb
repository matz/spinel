# A receiverless or `self.` instance-method list in a class body or a class method reads
# the version cells of a method a class method's body defines, as `K.instance_methods`
# does: the list agrees with the call it guards, for the class and for the subclasses a
# class method that takes its receiving class lists.
class K
  def self.setup = (def m = :k)
  def self.chk = instance_methods.include?(:m) ? new.m : :none
  def self.chk2 = self.public_instance_methods.include?(:m) ? new.m : :none
  p [(instance_methods.include?(:m) ? new.m : :none), chk, chk2]
end
p [K.chk, K.chk2]
K.setup
p [K.chk, K.chk2]

# a class method inherited by subclasses lists the class it runs on
class B
  def self.listed = instance_methods.include?(:q) ? new.q : :none
  def self.setup = (def q = :b)
end
class D < B
  def self.setup2 = (def q = :d)
end
p [B.listed, D.listed]
B.setup
p [B.listed, D.listed]
D.setup2
p [B.listed, D.listed]

# an included module's body def, listed from the class body
module Mod
  def self.setup = (def mm = :mm)
end
class Inc
  include Mod
  p [(instance_methods.include?(:mm) ? new.mm : :none)]
end
Mod.setup
p [Inc.instance_methods.include?(:mm), Inc.new.mm]

# a plain block in a class body keeps the body's self, so its list is the class's
class Blk
  def self.setup = (def bm = :bk)
  def n = 1
  p [1].map { instance_methods.include?(:n) }
  p [1].map { instance_methods.include?(:bm) ? new.bm : :none }
end
Blk.setup
p [Blk.instance_methods.include?(:bm), Blk.new.bm]

# a computed-name public_send in a module's method, run by the body of the class that extends it: the
# arm for `instance_methods` lists that class
module Lister
  def kind(k) = @kind = k
  def keep(c) = const_set(c, public_send(["instance", @kind].join("_")))
end
class Sent
  extend Lister
  def sm = 1
  kind :methods
  keep :LIST
end
p Sent::LIST.include?(:sm)
