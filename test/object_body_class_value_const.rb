# A class value a reopened Object's body writes to a constant, through a
# multiple assignment or `self::`, is a top-level constant: a read at the
# top level constructs it.
class Object
  X, v = Class.new { def hi = 2 }, 0
  p [v]
  self::Z, w = Class.new { def hi = 3 }, 1
end
p X.new.hi
p Z.new.hi
p X.new.class
p Z
