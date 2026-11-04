# super(a, &pr) with a proc value of the method's own: the parent's yield
# runs that proc, and takes its value (not the caller's block's).
class P
  def three(a)
    yield a
  end
end
class C < P
  def three(a, &blk)
    s = :to_s.to_proc
    super(a, &s)
  end
end
p C.new.three(5) { 0 }

class P2
  def three(a) = yield(a)
  def plus(a) = yield(a) + 1
  def twice(a) = [yield(a), yield(a + 1)]
end
class C2 < P2
  def three(a)
    s = :to_s.to_proc
    super(a, &s)
  end
  def plus(a)
    s = proc { |x| x * 2 }
    super(a, &s)
  end
  def twice(a, &blk) = super(a, &blk)
end
class D2 < P2
  def three(a) = super(a) { |x| x + 100 }
  def plus(a) = super(a, &->(x) { x * 3 })
end
class E2 < P2
  def three(a) = super(a, &:inspect)
  def plus(a)
    m = 10.method(:+)
    super(a, &m)
  end
end
p C2.new.three(5) { 0 }, D2.new.three(5), E2.new.three(:q)
p C2.new.plus(5), D2.new.plus(5), E2.new.plus(5)
p C2.new.twice(1) { |x| x.to_s * 2 }
x = C2.new.three(7)
p x.size
p P2.new.three(4) { |v| v * 2 }

class P3
  def run(a)
    r = yield(a)
    "got #{r}"
  end
end
class C3 < P3
  def run(a)
    pr = a > 1 ? proc { |x| x.to_s + "!" } : proc { |x| x }
    super(a, &pr)
  end
end
p C3.new.run(3), C3.new.run(1)
class Q
  def each_one(xs)
    xs.each { |x| yield x }
    :done
  end
end
class R < Q
  def each_one(xs)
    acc = []
    s = proc { |v| acc << v.to_s }
    super(xs, &s)
    acc
  end
end
p R.new.each_one([1, 2])
