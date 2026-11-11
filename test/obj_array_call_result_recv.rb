# spinel: gc-stress
# A method's value, or a filter's result, used straight as a receiver
# (`same(a).size`, `big(a).each { }`, `a.select { }.size`) or handed on as
# an argument (`same(same(a))`) is a use of the array it answers, as
# `b = same(a); b.size` is: an Array of objects or a table of rows keeps its
# typed form, and every call on it answers as CRuby.

class Pt
  attr_reader :v
  def initialize(v) = @v = v
end

class Qt
  attr_reader :v
  def initialize(v) = @v = v * 10
end

def same(a) = a
def big(a) = a.select { |x| x.v > 2 }

a = Array.new(6) { |i| Pt.new(i) }
p same(a).size, same(a)[2].v, same(a).first.v, same(a).last.v
same(a).each { |x| print x.v, " " }
puts
p same(a).map(&:v)
p big(a).size, big(a).first.v
p big(a).select { |x| x.v.odd? }.map { |x| x.v * 2 }
p a.reject { |x| x.v.even? }.size
same(a).reverse_each { |x| print x.v }
puts

same(a) << Pt.new(10)
p a.size, a.last.v
p same(a).pop.v, a.size
p big(a).empty?, big(big(a)).length
p same(same(a)).size, same(a.select { |x| x.v > 1 }).size

# a sort_by result handed on keeps its array boxed, and still answers
def other(c) = c
c = Array.new(3) { |i| Pt.new(i) }
p other(c.sort_by { |x| -x.v }).first.v

def rows(g) = g
g = Array.new(3) { |i| Array.new(3) { |j| i * 3 + j } }
p rows(g).size, rows(g)[1][2]
s = 0
i = 0
while i < 3
  r = g[i]
  j = 0
  while j < 3
    s += r[j]
    j += 1
  end
  i += 1
end
p s

def mixed(b) = b
b = [Pt.new(1), Qt.new(2)]
p mixed(b).size, mixed(b).map(&:v)
