# A boxed `recv.attr op= v` on a class with the reader and no writer reads the attribute, runs the
# operand and applies the operator before the NoMethodError for `attr=`, as CRuby does: an operand
# that raises, and an operator a nil slot cannot take (`nil + 1`), come first, and the operand runs once.
class R
  attr_reader :x
  def initialize(v) = @x = v
end
class W
  attr_accessor :x
  def initialize = @x = 1
end
def rhs = (puts "rhs"; 1)
[W.new, R.new(1), R.new(nil), nil].each do |r|
  begin
    r.x += rhs
    p r.x
  rescue NoMethodError => e
    p [e.name, e.receiver.class]
  end
end
[R.new(1), R.new(nil), nil].each do |r|
  begin
    r.x += rhs
    p r.x
  rescue NoMethodError => e
    p [e.name, e.receiver.class]
  end
end

class RO
  attr_reader :x, :y
  def initialize(a, b) = (@x = a; @y = b)
end
class WO
  attr_accessor :x, :y
  def initialize(a, b) = (@x = a; @y = b)
end
def two_rhs = (puts "rhs2"; 2)
def tail(r) = r.x += two_rhs
def two(r) = (r.x += two_rhs; r.y -= two_rhs)
[WO.new(1, 2), RO.new(1, 2), RO.new(nil, nil), nil].each do |r|
  begin
    p tail(r)
  rescue NoMethodError => e
    p [:tail, e.name]
  end
  begin
    p two(r)
  rescue NoMethodError => e
    p [:two, e.name]
  end
  begin
    v = (r.x += two_rhs)
    p v
  rescue NoMethodError => e
    p [:val, e.name]
  end
end

class RF
  attr_reader :f, :b
  def initialize = (@f = 2.5; @b = 6)
end
class WF
  attr_accessor :f, :b
  def initialize = (@f = 2.5; @b = 6)
end
[WF.new, RF.new].each do |r|
  begin
    r.f += rhs
    p [:f_add, :ok]
  rescue NoMethodError => e
    p [:f_add, e.name]
  end
  begin
    r.f *= rhs
    p [:f_mul, :ok]
  rescue NoMethodError => e
    p [:f_mul, e.name]
  end
  begin
    r.b |= rhs
    p [:b_or, :ok]
  rescue NoMethodError => e
    p [:b_or, e.name]
  end
  begin
    r.b <<= rhs
    p [:b_shl, :ok]
  rescue NoMethodError => e
    p [:b_shl, e.name]
  end
  begin
    r.f **= rhs
    p [:f_pow, :ok]
  rescue NoMethodError => e
    p [:f_pow, e.name]
  end
end
def boom = (raise "boom"; 1)
[WO.new(1, 2), RO.new(1, 2), nil].each do |r|
  begin
    r.x += boom
  rescue NoMethodError => e
    p [:nm, e.name]
  rescue => e
    p e.message
  end
end

# a bitwise operator on a nil slot of a reader-only class: `nil | 3` is a value, so the error is the
# writer's, and the operand ran once before it
class RB
  attr_reader :x
  def initialize(v) = @x = v
end
class WB
  attr_accessor :x
  def initialize(v) = @x = v
end
class NB; end
$bn = 0
def brhs(v) = ($bn += 1; v)
def run_or(list)
  list.each do |r|
    begin
      r.x |= brhs(3)
      p [:ok, r.x]
    rescue NoMethodError => e
      p [:nm, e.name, e.receiver.class]
    end
  end
  p $bn
end
def run_and(list)
  list.each do |r|
    begin
      r.x &= brhs(3)
      p [:ok, r.x]
    rescue NoMethodError => e
      p [:nm, e.name, e.receiver.class]
    end
  end
  p $bn
end
def run_xor(list)
  list.each do |r|
    begin
      r.x ^= brhs(3)
      p [:ok, r.x]
    rescue NoMethodError => e
      p [:nm, e.name, e.receiver.class]
    end
  end
  p $bn
end
run_or([WB.new(13), RB.new(13), RB.new(nil), nil, NB.new])
run_or([RB.new(13), RB.new(nil), nil])
run_or([RB.new(13), RB.new(7)])
run_and([WB.new(13), RB.new(13), RB.new(nil), RB.new(false), nil])
run_and([RB.new(nil), RB.new(true)])
run_xor([WB.new(13), RB.new(13), RB.new(nil), RB.new(true), nil])
run_xor([RB.new(nil), RB.new(false)])
[RB.new(8), RB.new(nil), nil].each do |r|
  begin
    r.x <<= brhs(1)
    p [:ok, r.x]
  rescue NoMethodError => e
    p [:nm, e.name, e.receiver.class]
  end
  begin
    r.x >>= brhs(1)
    p [:ok, r.x]
  rescue NoMethodError => e
    p [:nm, e.name, e.receiver.class]
  end
end
p $bn

# a String slot of a reader-only class that holds nil: `nil + "b"` raises for `+` before the
# writer's error, as `s + t` does; a String slot that holds a String reaches the writer's
class RS
  attr_reader :str
  def initialize(v) = @str = v
end
class WS
  attr_accessor :str
  def initialize(v) = @str = v
end
[WS.new("a"), RS.new("a"), RS.new(nil), nil].each do |r|
  begin
    r.str += "b"
    p [:ok, r.str]
  rescue NoMethodError => e
    p [:nm, e.name, e.receiver.class]
  end
end

# an operand that needs a prelude (an Array literal, an interpolation, a Hash) runs once, in the case
# that runs: not for a nil receiver, and not once for every reader-only class of the switch
class RQ1
  attr_reader :q
  def initialize(v) = @q = v
end
class RQ2
  attr_reader :q
  def initialize(v) = @q = v
end
class WQ
  attr_accessor :q
  def initialize(v) = @q = v
end
$qn = 0
def eff = ($qn += 1; puts "eff #{$qn}"; 1)
[WQ.new(1), RQ1.new(1), RQ2.new(2), RQ1.new(nil), nil].each do |r|
  begin
    r.q += [eff].first
    p [:ok, r.q]
  rescue NoMethodError => e
    p [:nm, e.name, e.receiver.class]
  end
end
p $qn
[RQ1.new(1), RQ2.new(2), nil].each do |r|
  begin
    r.q += "#{eff}".to_i
    p [:ok, r.q]
  rescue NoMethodError => e
    p [:nm, e.name, e.receiver.class]
  end
end
p $qn
[RQ1.new(1), nil].each do |r|
  begin
    r.q += {a: eff}[:a]
  rescue NoMethodError => e
    p [:nm, e.name, e.receiver.class]
  end
end
p $qn
