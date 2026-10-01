# An Enumerable #each whose yields pass different numbers of values, or a
# splat: Enumerable packs a yield of several values into one Array element
# and leaves a single value as it is. The collector took as many parameters
# as the widest yield and always packed, so `yield 0` became [0, nil], and a
# splat counted as one value.

class Y1
  include Enumerable
  def each
    yield 0
    yield 0, 1
  end
end
class Y2
  include Enumerable
  def each
    yield 0
    yield(*[0, 1, 2])
  end
end
class Y3
  include Enumerable
  def each
    yield
    yield 1
    yield 2, 3
    yield [4, 5]
  end
end
class Y4
  include Enumerable
  def each
    yield 1, 2
    yield 3, 4
  end
end
class Y5
  include Enumerable
  def each(&b)
    b.call(1)
    b.call(2, 3)
  end
end
p Y1.new.to_a, Y2.new.to_a, Y3.new.to_a, Y4.new.to_a, Y5.new.to_a
p Y3.new.select { |x| x }, Y1.new.include?([0, 1]), Y3.new.first(3), Y2.new.count
p Y1.new.map { |x| x }, Y2.new.map { |x| x }, Y4.new.map { |x| x }
for x in Y2.new do p x end
for x in Y1.new do p x end
