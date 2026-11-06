# A builtin class's builtin modules (Time: Comparable; IO, Dir:
# Enumerable) answer is_a?, ===, include? and ancestors, and a `when` or
# `in` arm with the module matches an object of a builtin subclass.
module M; def mm = 1; end
class Time; include M; end
class SubA < Array; end
class SubH < Hash; end
class SubS < String; end
class SubT < Time; end
St = Struct.new(:a)

def kind(x)
  case x
  when Comparable then :cmp
  when Enumerable then :enum
  else :other
  end
end
p [kind(SubA[1]), kind(SubH[1, 2]), kind(SubS.new("a")), kind(SubT.at(0)), kind(St.new(1))]

case SubA[1] when Enumerable then p :enum else p :other end
case SubH[1, 2] when Enumerable then p :enum else p :other end
case SubS.new("x") when Comparable then p :cmp else p :other end
case SubT.at(0) when Comparable then p :cmp else p :other end
case SubT.at(0) when M then p :m else p :other end
case SubS.new("q") when Enumerable then p :enum when Kernel then p :kernel else p :other end
r = case SubA[1] when Comparable then :cmp when Enumerable then :enum else :other end
p r
case SubT.at(0)
in Enumerable then p :enum
in Comparable then p :cmp
end
x = ARGV.size > 5 ? SubA[1] : nil
case x when Enumerable then p :enum when nil then p :nil else p :other end
y = ARGV.size > 5 ? nil : SubS.new("a")
case y when Comparable then p :cmp else p :other end

p SubT.at(0).is_a?(Comparable), SubT.at(0).kind_of?(Comparable), Comparable === SubT.at(0)
p Time.now.is_a?(Comparable), Comparable === Time.now
case Time.now when Comparable then p :cmp else p :other end
p St.new(1).is_a?(Enumerable), Enumerable === St.new(1)
p $stdout.is_a?(Enumerable), Enumerable === $stdout
case $stdout when Enumerable then p :enum else p :other end
p [SubA[1], SubT.at(0), 3, "s"].map { |v| v.is_a?(Comparable) }
p [SubA[1], SubT.at(0), 3, "s"].map { |v| case v when Enumerable then 1 when Comparable then 2 else 0 end }

p Time.include?(Comparable), SubT.include?(Comparable), SubT <= Comparable
p IO.include?(Enumerable), File.include?(Enumerable), Dir.include?(Enumerable)
p Time.ancestors.take(4), SubT.ancestors.take(5), Dir.ancestors.take(3)

module Ns
  module Enumerable; end
  def self.t(x) = case x when Enumerable then :ns_enum else :other end
  def self.u = case SubA[1] when Enumerable then :ns_enum else :other end
end
p Ns.t(SubA[1]), Ns.u

# a File.stat result and a nil IO slot are no Enumerable
st = File.stat(__FILE__)
case st when Enumerable then p :enum else p :other end
p(case st
  in Enumerable then :enum
  else :other
  end)
f = nil
case f when Enumerable then p :enum else p :other end
g = ARGV.size > 5 ? File.open(__FILE__) : nil
case g when Enumerable then p :enum when nil then p :nil else p :other end
