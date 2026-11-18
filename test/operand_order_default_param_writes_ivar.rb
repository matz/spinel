# An operand read ahead of a call whose callee's default parameter writes
# the read variable keeps the value it had: the default runs where the call
# does, after the operands to its left, as in CRuby (#11506).
class P
  def initialize; @i = 1; end
  def f(x, y) = [x, y]
  def set(v) = @i = v
  def nxt(step = set(6)) = step
  def two(a, b = set(a + 1)) = b
  def run = f(@i, nxt)
  def given = f(@i, nxt(2))
  def after_required = f(@i, two(9))
end
p P.new.run
p P.new.given
p P.new.after_required
q = P.new
p [q.instance_variable_get(:@i), q.run, q.instance_variable_get(:@i)]
