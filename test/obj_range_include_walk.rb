# Range#include? / #member? on program objects: CRuby walks by succ and
# compares with ==, refuses an open end, and answers by cover? only for a
# Numeric or an end answering to_int.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = V.new(n + 1)
end

class N < Numeric
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = N.new(n + 1)
end

class T
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ = T.new(n + 1)
  def to_int = n
end

r = V.new(1)..V.new(5)
p r.include?(V.new(3))
p r.include?(V.new(2.5))
p r.member?(V.new(2.5))
p r === V.new(2.5)
p r.cover?(V.new(2.5))
p (V.new(1)...V.new(5)).include?(V.new(5))
p (V.new(1)..V.new(5)).include?(V.new(5))
[(V.new(1)..), (..V.new(5))].each do |o|
  begin
    p o.include?(V.new(3))
  rescue TypeError => e
    p [e.class, e.message]
  end
  p o === V.new(3)
end
p (N.new(1)..N.new(5)).include?(N.new(2.5))
p (N.new(1)..).include?(N.new(2.5))
p (T.new(1)..T.new(5)).include?(T.new(2.5))

boxed = [V.new(1)..V.new(5), 1..5, "a".."e"]
boxed.each { |b| p b.include?(b.first.succ) }
p boxed[0].include?(V.new(2.5))
p boxed[0].member?(V.new(2.5))
