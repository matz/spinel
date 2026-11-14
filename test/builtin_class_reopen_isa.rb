# A program that reopens a builtin class (class Module; def m; end) gets a
# class entry for it. A builtin value (String, Comparable, a Method, 5)
# carries its own tag or id, not that entry, so is_a?, instance_of?, ===,
# `when` and `in` on a boxed value ask the runtime too, by the class's Ruby
# name (Enumerator::Chain), and so does a class held in a value.
class Module; def mx = 1; end
class Method; def mtx = 1; end
class UnboundMethod; def ux = 1; end
class BasicObject; def bx = 1; end
class Data; def dx = 1; end
class Random; def rx = 1; end
class Enumerator::Chain; def cx = 1; end

P = Data.define(:a)
class Foo < Module; end

w = [String, Comparable, Foo.new, 1.method(:+), P.new(a: 1), Random.new(1), 5, "s", nil]
p w.map { |x| [x.is_a?(Module), x.instance_of?(Module), x.kind_of?(Method), x.instance_of?(Method)] }
p w.map { |x| [x.is_a?(Data), x.instance_of?(Data), x.is_a?(Random), x.instance_of?(Random)] }
p w.map { |x| [Module === x, Method === x, Data === x, Random === x, BasicObject === x] }
p w.map { |x| case x when Method then 1 when Data then 2 when Random then 3 when Module then 4 when BasicObject then 5 else 0 end }
r = w.map do |x|
  case x
  in Method then 1
  in Data then 2
  in Module then 4
  else 0
  end
end
p r

v = [String, 5][ARGV.size]
p v.is_a?(Module), v.is_a?(BasicObject), v.instance_of?(Class)

# a nested builtin class, and an UnboundMethod
e = [[1].each.chain([2]), [3].each, 1.method(:+).unbind, 5]
p e.map { |x| [x.is_a?(Enumerator::Chain), Enumerator::Chain === x, (case x when Enumerator::Chain then 1 else 0 end), x.is_a?(UnboundMethod), UnboundMethod === x] }

# a class held in a value, and the Enumerable calls that take one
[Method, Random, Module].each { |k| p w.map { |x| [x.is_a?(k), k === x] } }
p w.grep(Method).size, w.grep(Random).size, w.any?(Random), w.count { |x| Random === x }

# a program's own class of the same leaf name does not take builtin values
module Ns
  class Method; end
  def self.check(xs) = xs.map { |x| [x.is_a?(Ns::Method), (case x when Ns::Method then 1 else 0 end)] }
end
p Ns.check([Ns::Method.new, 1.method(:+), 5])
