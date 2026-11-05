# A Range of objects that is only ever boxed still walks by its class's succ
# (the method is kept for the boxed walk); the walk calls succ on a member only
# after the block ran for it; a boxed one answers first; `|x, *rest|` binds rest
# empty; and overlap? on a receiver that is no Range is NoMethodError.
class V
  include Comparable
  attr_reader :n
  def initialize(n) = @n = n
  def <=>(o) = n <=> o.n
  def succ
    puts "succ #{n}"
    V.new(n + 1)
  end
end

b = [V.new(1)..V.new(3), 1][0]
b.each { |v| puts "body #{v.n}"; break if v.n == 2 }
p b.first.n
p b.find { |v| v.n == 2 }.n
(V.new(1)..V.new(2)).each { |x, *rest| p [x.n, rest] }
a = [1, V.new(1)..V.new(2)][0]
begin
  a.overlap?(V.new(1)..V.new(2))
rescue NoMethodError
  puts "NoMethodError"
end
