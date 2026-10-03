# A super into a yielding parent that also leaves through `return` is
# spliced as one without: its returns end the inlined body, and the super
# answers them or the yield's value. It was called as a function the parent
# never has, and the program did not link. A super with a block of its own
# does not make the method that holds it yielding, so a `return` in that
# block leaves that method, not its caller.
class P
  def self.foo(a)
    return yield(a) if a > 0
    -1
  end

  def run(a)
    return :neg if a < 0
    r = yield(a)
    return :big if r > 100
    r
  end

  def each_v(xs)
    xs.each { |x| return x if x > 2; yield x }
    :done
  end
end

class Q < P
  def self.foo(a) = super(a + 1) { |x| x * 2 }
  def run(a) = super(a) { |x| x * 10 }

  def each_v(xs)
    acc = []
    r = super(xs) { |x| acc << x }
    [r, acc]
  end
end

class Q2 < P
  def run(a)
    super(a) { |x| return :early if x == 7; x }
    :after
  end
end

class R < P
  def run(a)
    v = super
    [:r, v]
  end
end

p Q.foo(3), Q.foo(-5)
q = Q.new
p q.run(3), q.run(-1), q.run(20)
p Q2.new.run(7), Q2.new.run(1), Q2.new.run(-3)
p q.each_v([1, 2, 3, 4]), q.each_v([1])
p R.new.run(5) { |x| x + 1 }, R.new.run(-2) { |x| x }
p :end
