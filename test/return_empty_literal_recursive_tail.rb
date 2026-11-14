# An early `return []` / `return {}` beside a tail that waits on the
# method's own value: the empty literal is the value, not nil.
def h1(d)
  return {} if d == 0
  h1(d - 1).merge(d => d * 2)
end
p h1(3)
def h2(d)
  return {} if d == 0
  r = h2(d - 1); r["k#{d}"] = d; r
end
p h2(2)
def m1(d)
  return [] if d <= 0
  m2(d - 1) + [d]
end
def m2(d)
  return [] if d <= 0
  m1(d - 1) + ["s#{d}"]
end
p m1(4)
class T
  def self.walk(n)
    return [] unless n
    walk(n[1]) + [n[0]]
  end
  def flat(a)
    return [] if a.empty?
    flat(a.drop(1)).unshift(a.first)
  end
end
p T.walk([1, [2, [3, nil]]])
p T.new.flat([4, 5, 6])
def e1(xs)
  xs.each { |x| return [] if x < 0 }
  e1(xs.map { |x| x - 1 }) + [xs.size]
end
p e1([2, 3])
def s1(d)
  return [] if d == 0
  s1(d - 1).push(d.to_s)
end
p s1(3)
p s1(3).map(&:upcase)
def f1(d)
  return [] if d == 0
  [d * 1.5] + f1(d - 1)
end
p f1(2).sum
def ymeth(a); yield a; end
def callself(depth)
  return [] if depth == 0
  ymeth(depth) { |x| callself(depth - 1) + [x] }
end
p callself(3)
