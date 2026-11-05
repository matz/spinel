# A program's own Integer#step, #times, #upto or #downto with a splat or a
# block parameter replaces the builtin, also for a call with a block.
class Integer
  def step(*a) = a.inspect
  def times(*a) = yield(a.size + self)
  def upto(lim, *rest, &b) = [self, lim, rest, b ? b.call(0) : :nob]
  def downto(n, &b) = [n, b ? b.call(1) : :nob]
end
p 1.step(3, 1) { }
p 1.step { }
p 1.step(1, 2, 3)
p 3.times { |x| x * 10 }
p 3.times(4, 5) { |x| x }
p 1.upto(3) { |x| x + 9 }
p 1.upto(3, 4)
p 5.downto(2) { |x| x * 7 }
p 5.downto(2)
