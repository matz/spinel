# A Range of objects walks its members by succ while <=> keeps them within
# the end: Dates (boxed) and a class of the program's (typed), through each
# and the Enumerable methods, and CRuby's TypeError for a class with no succ
require "date"
r = (Date.new(2026, 1, 30)..Date.new(2026, 2, 2))
r.each { |d| puts d }
p r.to_a.size
p r.entries.size
p r.map(&:day)
p r.select { |d| d.day.even? }.map(&:day)
p r.reject { |d| d.day.even? }.size
p r.count
p r.count { |d| d.month == 2 }
p r.each_with_index.map { |d, i| d.day + i }
p r.inject(0) { |s, d| s + d.day }
p r.group_by(&:month).keys
p r.first(2).map(&:day)
p r.min_by(&:day).day
p r.each_slice(3).map(&:size)
p r.any? { |d| d.day == 1 }
p (Date.new(2026, 1, 1)...Date.new(2026, 1, 4)).map(&:day)
p (Date.new(2026, 1, 4)..Date.new(2026, 1, 1)).to_a

class Ver
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = Ver.new(n + 1)
  def to_s = "v#{n}"
end
v = (Ver.new(1)..Ver.new(4))
p v.map(&:n)
v.each { |x| print x, " " }
puts
p (Ver.new(1)...Ver.new(3)).to_a.map(&:to_s)
p v.sort_by { |x| -x.n }.map(&:n)

class NoSucc
  def <=>(o) = 0
end
begin
  (NoSucc.new..NoSucc.new).each { |x| p x }
rescue TypeError => e
  p e.message
end
