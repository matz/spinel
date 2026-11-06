# Edges of Ranges of Times and of objects: a beginless count, a step that
# cannot move over (t..t), the inclusive end as the last step, a rest-only
# block, a break value, a Time beside another end, a clamp of the program's
# own, ends that do not compare, and when arms over object Ranges
t = Time.at(0).utc
p((..t).count)
p((t..t).step(0).to_a.size)
p((t...t).step(0).to_a.size)
p((t..t).step(0.0 / 0).to_a.size)
p((t..t + 1).step(0).to_a.size)
p((t..t).step(Float::INFINITY).to_a.size)
n = 0
(t..t).step(0) { |x| n += 1 }
p n
rest = []
(t..t + 2).step(1) { |*xs| rest << xs.size }
p rest
v = (t..t + 5).step(1) { |x| break 7 if x > t + 1 }
p v

ends = [Time.at(0), 5]
begin
  p((ends[0]..ends[1]).size)
rescue ArgumentError => e
  p e.message
end

class Sink
  def clamp(r) = r.begin
end
p Sink.new.clamp(Time.at(0).utc..Time.at(1).utc).to_i

class Ver
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = o.is_a?(Ver) && n.positive? && o.n.positive? ? n <=> o.n : nil
  def to_s = "v#{n}"
  def hash = n.hash
  def eql?(o) = o.is_a?(Ver) && n == o.n
end
begin
  bad = (Ver.new(-1)..Ver.new(2))
  p bad.begin.n
rescue ArgumentError => e
  p e.message
end
r = (Ver.new(1)..Ver.new(5))
p({ r => 1 }[(Ver.new(1)..Ver.new(5))])
case (Ver.new(1)..Ver.new(2))
when r then p :range_subject_matched
else p :range_subject_no
end
case Ver.new(3)
when r then p :covered
end
boxed = [r, 1][0]
p boxed.cover?(*[Ver.new(2)])
p boxed.cover?(Ver.new(9))
