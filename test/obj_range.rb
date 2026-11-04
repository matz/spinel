# A Range of the program's objects whose class defines <=> (a Date, a
# Comparable class): its ends, membership, equality, hashing, case/when and
# rendering, typed and boxed.
require "date"
a = Date.new(2026, 1, 1); b = Date.new(2026, 1, 31); m = Date.new(2026, 1, 15)
r = a..b
p r
puts r
p r.begin, r.end.day, r.exclude_end?
p r.cover?(m), r.cover?(Date.new(2026, 2, 1)), r.include?(m), r === b, (a...b).cover?(b)
p r == (a..b), r.eql?(a...b), r.hash == (a..b).hash
p (a..).cover?(m), (..a).cover?(m)
p r.min, r.max
case m
when r then puts "in"
else puts "out"
end
h = { r: r, n: 1 }
p h[:r].cover?(m), h[:r].begin
p({ r => 1 }[a..b])
p r.cover?(a..m)
class Ver
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def to_s = "v#{n}"
  def inspect = "#<Ver #{n}>"
end
vr = Ver.new(1)..Ver.new(5)
p vr, vr.cover?(Ver.new(3)), vr.cover?(Ver.new(9))
