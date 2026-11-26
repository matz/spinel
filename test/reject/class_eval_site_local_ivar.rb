# spinel: reject-unsupported: (CallNode `class_eval`)
# An ivar in a class_eval block on a local holding a class a Class.new
# makes at each evaluation is that class's own; the classes one call makes
# share one set of class-level ivars, so such a block is not spliced and
# stays refused, the write and the read alike.
def mk(n)
  k = Class.new { def v = 1 }
  k.class_eval { @civ = n }
  [k.class_eval { @civ }, @civ.inspect]
end
p mk(1), mk(2)
