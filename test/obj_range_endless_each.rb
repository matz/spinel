# A Range of objects walks by succ a member at a time: an endless one stops
# where the block breaks, and each answers the Range
require "date"
class V
  include Comparable
  attr_reader :n
  def initialize(n); @n = n; end
  def <=>(o); o.is_a?(V) ? n <=> o.n : nil; end
  def succ; V.new(n + 1); end
  def inspect; "V#{n}"; end
end

r = V.new(1)..V.new(3)
seen = []
ret = r.each { |x| seen << x }
p seen, ret.equal?(r)
got = []
(V.new(5)..).each { |x| break if x.n > 7; got << x }
p got
p((V.new(1)..).each { |x| break x if x.n == 4 })
p (V.new(2)..).first(3), (V.new(2)...V.new(3)).first(5), r.first(0)
nexts = []
(V.new(1)..V.new(4)).each { |x| next if x.n.even?; nexts << x }
p nexts

d = Date.new(2026, 11, 28)
days = []
(d..).each { |x| break if x.month == 12 && x.day > 2; days << x.day }
p days
p (d..).first(2).map(&:day), (d..Date.new(2026, 11, 30)).each { }.class
p (d..).find { |x| x.wday == 0 }.day
begin
  (V.new(1)..).first(-1)
rescue ArgumentError => e
  p e.message
end
