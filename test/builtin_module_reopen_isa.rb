# A program that reopens a builtin module (module Comparable; def m; end)
# gets a class entry for it. A builtin value (an Integer, a String, an
# Array) that includes the module carries no program class id, so is_a?
# and `when` on a boxed value ask the runtime too.
module Kernel; def kx = 1; end
module Enumerable; def ex = 1; end
module Comparable; def cx = 1; end

class P; include Comparable; def <=>(o) = 0; end
class Q; include Enumerable; def each = yield(1); end

w = [3, "s", [1], {a: 1}, 1..2, P.new, Q.new, nil, :s, 2.5, 2**80]
p w.map { |x| [x.is_a?(Comparable), x.is_a?(Enumerable), x.is_a?(Kernel), x.kind_of?(Comparable)] }
p w.map { |x| case x when Comparable then 1 when Enumerable then 2 when Kernel then 3 else 0 end }
p w.map { |x| [Comparable === x, Enumerable === x] }
p w.map { |x| x.instance_of?(Comparable) }

v = [3, "s", [1]][ARGV.size]
p v.is_a?(Comparable), v.is_a?(Enumerable), v.is_a?(Kernel)
p(case v when Comparable then 1 when Enumerable then 2 else 0 end)

# a Struct, a Rational, a Complex and an Enumerator too
St = Struct.new(:a)
y = [St.new(1), 1r, Complex(1, 2), (1..2).each]
p y.map { |e| [e.is_a?(Comparable), e.is_a?(Enumerable), (case e when Enumerable then 1 when Comparable then 2 else 0 end)] }

# the reopened methods still work
p 3.cx, 2.5.cx, [1].ex, {a: 1}.ex, 3.kx, P.new.cx, Q.new.ex

# a program's own module of the same leaf name does not take builtin values
module Ns
  module Comparable; def nx = 1; end
  class R; include Ns::Comparable; end
  def self.check(xs) = xs.map { |x| [x.is_a?(Ns::Comparable), (case x when Ns::Comparable then 1 else 0 end)] }
end
p Ns.check([3, "s", Ns::R.new, nil])
