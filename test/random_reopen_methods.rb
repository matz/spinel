# Methods a program adds to Random by reopening it run on the runtime's
# generator: Random.new keeps its arguments and builtins, and self inside
# the reopening is the generator.
class Random; def tag = :t; end
p Random.new(1).tag
r = Random.new(42)
p r.rand(100) == Random.new(42).rand(100)
p Random.new.tag
p r.seed
class Random
  def roll(n) = rand(n) + 1
  def me = self
end
r2 = Random.new(7)
p r2.roll(6).between?(1, 6)
p r2.me.equal?(r2)
p((5.tag rescue $!.class))
