# Issue #49: a class with `def initialize(start = 0)` should compile
# both `Counter.new(0)` and `Counter.new` — the no-arg call site needs
# the default value substituted in.

class Counter
  def initialize(start = 0)
    @n = start
  end
  def n
    @n
  end
end

c1 = Counter.new
puts c1.n        # 0 (default)

c2 = Counter.new(7)
puts c2.n        # 7
