# A yielding Array reopen method reached through a forwarded `&b`, from a
# method called with a block and without one: no block reaches the
# reopen as NULL, which its block_given? reads as false.
class Array
  def in_groups_of(n)
    out = []
    each_slice(n) { |g| out << g }
    block_given? ? out.each { |g| yield g } : out
  end
end
class Runner
  def initialize(xs) = @xs = xs
  def run(&b)
    @xs.in_groups_of(2, &b)
  end
end
r = Runner.new([1, 2, 3])
p r.run
r.run { |g| p g }
rs = [Runner.new([4]), Runner.new([5, 6, 7])]
rs.each { |x| x.run { |g| p [:g, g] } }
rs.each { |x| p x.run }
def rec(n, &b)
  return [n].in_groups_of(1, &b) if n <= 0
  rec(n - 1, &b)
end
p rec(2)
rec(2) { |g| p [:rec, g] }
