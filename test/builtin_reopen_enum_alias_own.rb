# A reopened Array, Range or Hash (or a module mixed into one) with its own
# find_all, collect_concat or detect: the call runs that method, not the
# builtin's select/flat_map/find.
module Finder
  def find_all = "find-mod"
  def collect_concat = "cc-mod"
end
class Array; include Finder; end

class Range
  def find_all(&b) = "range-fa:#{b ? b.call(1) : nil}"
  def collect_concat = "range-cc"
  def detect = "range-det"
end

class Hash
  def find_all = "hash-fa"
  def collect_concat = "hash-cc"
end

class Foo
  def find_all = "foo-fa"
  def collect_concat = "foo-cc"
end

class Sub < Array; end

p [[1], [2]].find_all
p [[1], [2]].collect_concat
p Sub.new([1]).find_all
p Sub.new([1]).collect_concat
p (1..3).find_all { |x| x * 10 }
p (1..3).find_all
p (1..3).collect_concat
p (1..3).detect
p({a: 1}.find_all)
p({a: 1}.collect_concat)
v = [[1], Foo.new][ARGV.size + 1]
p v.find_all, v.collect_concat
def m(a) = a.find_all
p m([3])
p [[1]].flat_map { |a| a }
p [1, 2].select { |x| x > 1 }
# with a block too
module M2; def find_all = "m2-fa"; end
class Mx < Array; include M2; end
p Mx.new([1, 2]).find_all { |x| x > 1 }
p [1, 2].find_all { |x| x > 1 }
def mb(a) = a.find_all(&:odd?)
p mb([1, 2])
