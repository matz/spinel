# --warn-widen's why: a send on a poly receiver is followed to the candidate
# whose return degraded, not to the receiver; an empty literal filled with
# objects (its value goes on to a sort_by) is untyped by representation, not
# by the round; and a return that met two kinds names the other `return`.
class Bad
  def initialize(v) = @v = v
  def to_s = @v
end
def mk(n) = n > 5 ? 1 : "x"
def label(v) = v.to_s
class Foo
  def initialize(i) = @i = i
end
def load(n)
  results = []
  k = 0
  while k < n
    results << Foo.new(k)
    k += 1
  end
  results
end
# (nil meeting a Symbol widens; meeting a String, an Integer or a Float it
# is the nullable form of that kind, #4567)
def pick(n)
  return nil if n > 5
  return :y if n > 3
  :x
end
b = Bad.new(mk(ARGV.length))
puts label(mk(ARGV.length)), b.to_s.to_s, load(ARGV.length).sort_by(&:object_id).length, pick(ARGV.length).to_s
