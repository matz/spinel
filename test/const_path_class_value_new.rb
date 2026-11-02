# A class value a multiple assignment writes to a module's constant is
# constructed through the constant's path, as through its bare name inside
# the module.
module M
  X, v = Class.new { def hi = 2 }, 0
  Y, = Class.new
  p X.new.hi
end
p M::X.new.hi
p M::X.new.class
p M::Y.new.class
p ::M::X.new.hi
