# A user reader named like a builtin's method (Random#seed) answers an
# Integer that can be nil; a call of it on a receiver that is only nil is
# emitted through the builtin's face, whose plain Integer answer is wrapped
# into the call's nil-holding form where the call's reader takes that form.
class Cfg
  attr_accessor :seed
  def initialize
    @seed = 42
  end
end

class Ctx
  attr_accessor :rng_state
  def initialize(ids, seed)
    @ids = ids
    @rng_state = seed
  end
end

def generate(ids, cfg = nil)
  ctx = nil
  if cfg != nil
    ctx = Ctx.new(ids, cfg.seed)
  end
  ids.length
end

def bump(cfg = nil)
  return -1 if cfg.nil?
  cfg.seed + 1
end

puts generate([1, 2, 3])
puts bump
