# A block parameter that is stored, aliased, reassigned or called with a
# splat is still called, so the blocks given to its method reach what that
# call answers.

class Ctx
  def initialize(n) = @n = n
  def n = @n
end

module Keep
  SLOTS = {}
  def self.run(key, &block)
    @last = block
    SLOTS[key] = block.call
  end

  def self.later(&block)
    @later = block
    nil
  end

  def self.again = @later.call

  def self.aliased(&block)
    pr = block
    pr.call
  end

  def self.defaulted(key, &block)
    block ||= proc { 0 }
    SLOTS[key] = block.call
  end

  def self.splat(key, *args, &block) = SLOTS[key] = block.call(*args)
end

Keep.run(:a) { 3 }
Keep.run(:b) { Ctx.new(2) }
p Keep::SLOTS[:a]
p Keep::SLOTS[:b].n

Keep.later { "s" }
p Keep.again
Keep.later { Ctx.new(4) }
p Keep.again.n

p Keep.aliased { 1 }
p Keep.aliased { Ctx.new(6) }.n

Keep.defaulted(:c) { 3 }
Keep.defaulted(:d) { Ctx.new(8) }
p Keep::SLOTS[:d].n

Keep.splat(:e, 1) { |x| x }
Keep.splat(:f, 10) { |x| Ctx.new(x) }
p Keep::SLOTS[:e]
p Keep::SLOTS[:f].n
