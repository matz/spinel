# A Range or Time reopening's yielding method runs with the call's block,
# and a Float's or Symbol's is called without one.
class Range
  def ends
    yield first, last
    self
  end

  def each_twice
    each { |v| yield v * 2 }
    nil
  end

  def maybe
    block_given? ? yield(size) : -1
  end
end

class Time
  def with_secs
    yield to_i
  end
end

r = (1..3)
r.ends { |a, b| p [a, b] }
p((4..5).ends { |a, b| a + b })
got = []
r.each_twice { |v| got << v }
p got
p r.maybe { |n| n * 10 }
p r.maybe

t = Time.at(100)
p t.with_secs { |s| s + 1 }
total = 0
t.with_secs { |s| total += s }
p total

class Float
  def maybe
    block_given? ? yield(1) : -1
  end
end

class Symbol
  def maybe
    block_given? ? yield(to_s) : :none
  end
end

p 1.5.maybe { |n| n * 10 }
p 1.5.maybe
p :a.maybe { |s| s * 2 }
p :a.maybe
