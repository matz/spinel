# A Range of Times is a Range of its own (sp_TimeRange): its ends, membership,
# equality, case/when and rendering answer as CRuby's, and what would iterate
# raises CRuby's TypeError.
a = Time.utc(2026, 1, 1); b = Time.utc(2026, 2, 1); m = Time.utc(2026, 1, 15)
r = a...b
p r
puts r
puts r.to_s
p r.class
p r.begin
p r.end.month
puts r.first.day
p r.exclude_end?
p r.cover?(m), r.cover?(b), (a..b).cover?(b), r === m, r.include?(m), r.member?(a)
p r == (a...b), r.eql?(a..b), r.hash == (a...b).hash
e = (a..)
p e, e.cover?(b), e.end
s = ..b
p s, s.cover?(a), s.begin
p r.min
p (a..b).max
begin
  r.max
rescue TypeError => ex
  puts "TypeError: #{ex.message}"
end
begin
  r.to_a
rescue TypeError => ex
  puts "TypeError: #{ex.message}"
end
p r.frozen?
case m
when r then puts "in"
else puts "out"
end
case b
when r then puts "in"
else puts "out"
end
p r.cover?(a...m)
p r.cover?(5)
h = { range: r }
p h[:range].cover?(m)
arr = [r, a..m]
p arr.map { |x| x.cover?(m) }
def within(range, t) = range.cover?(t)
p within(r, m)
class Holder
  attr_reader :span
  def initialize(span) = @span = span
end
p Holder.new(r).span.end
p r.begin.month + 1
