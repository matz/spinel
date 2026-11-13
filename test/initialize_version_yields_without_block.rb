# A `new` before a reopening runs the earlier initialize, which yields: with
# no block it runs that version (a yield raises LocalJumpError), with a
# literal block it yields to it, and on a proc (`&pr`) it runs that version
# on the proc.

class E1 < StandardError
  def initialize(m = "one")
    super(block_given? ? yield(m) : m)
  end
end
p E1.new.message
begin; raise E1; rescue E1 => e; p e.message; end
p E1.new { |m| m.upcase }.message
p E1.exception.message
begin; raise E1, "msg"; rescue E1 => e; p e.message; end
class E1
  def initialize(m = "two") = super(m + "!")
end
p E1.new.message
begin; raise E1; rescue E1 => e; p e.message; end
p E1.exception.message
begin; raise E1, "msg"; rescue E1 => e; p e.message; end

class Y1
  attr_reader :r
  def initialize(a = 1) = (@r = yield(a))
end
begin; Y1.new; rescue LocalJumpError => e; p e.class.name; end
p Y1.new { |x| x + 10 }.r
class Y1
  def initialize(a = 2) = (@r = a * 5)
end
p Y1.new.r

class Y4
  attr_reader :r
  def initialize(a = 1) = (@r = yield(a))
end
begin; Y4.new; rescue LocalJumpError => e; p e.class.name; end
class Y4
  def initialize(a = 2) = (@r = a * 5)
end
p Y4.new.r

# a literal block at the earlier version: block_given? is true there
class Y5
  attr_reader :r
  def initialize(a = 1) = (@r = block_given? ? yield(a) : :none1)
end
p Y5.new.r, Y5.new { |x| x + 1 }.r
class Y5
  def initialize(a = 2) = (@r = block_given? ? yield(a * 10) : :none2)
end
p Y5.new.r, Y5.new { |x| x + 1 }.r

class Y2
  attr_reader :r
  def initialize = (@r = yield(1))
end
pr = proc { |x| x.to_s + "!" }
p Y2.new(&pr).r
class Y2
  def initialize = (@r = yield(2) * 3)
end
p Y2.new(&pr).r

class Y3
  attr_reader :r
  def initialize = (@r = block_given? ? yield(1) : :none)
end
pq = proc { |x| x * 100 }
p Y3.new(&pq).r, Y3.new.r
class Y3
  def initialize = (@r = block_given? ? yield(2) : :none2)
end
p Y3.new(&pq).r, Y3.new.r
