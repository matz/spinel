# `self.dup`, `self.clone` and `self.freeze` at main call the builtin until a class Object or a
# module it includes defines the method: only the later calls answer the program's method.

p self.dup.class
p self.frozen?
class Object
  def dup = :mine_dup
end
p self.dup
module Fz
  def freeze = :mine_freeze
end
class Object
  include Fz
end
p self.freeze
def d = dup
p d
