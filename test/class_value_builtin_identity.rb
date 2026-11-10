# A Class value known only at run time that is a builtin class, a builtin
# the program reopens, or a program subclass of one: identity, predicates
# and the class methods the program gives it.
class Object; def self.oh = :oh; end
class Array; def self.ah = :ah; X = 1; end
class Hash; def self.hh = :hh; end
class MyA < Array; def self.ma = :ma; end
class MyH < Hash; end
class Foo; def self.fh = :fh; end

def kind(k, o) = [o.is_a?(k), o.kind_of?(k), k === o, (case o when k then :y else :n end)]
NAMES = { Array => :array, Hash => :hash, MyA => :mya, Foo => :foo }
def name_of(k) = NAMES.fetch(k, :other)

xs = [[], {}, MyA.new, MyH.new, Foo.new, 1, "s", nil]
xs.each do |x|
  k = x.class
  p [k, k.name, k == x.class, k.object_id == x.class.object_id, name_of(k)]
  p xs.map { |o| kind(k, o) }
end

a = [[], {}][ARGV.size]
k = a.class
p k == Array, k.equal?(Array), k.object_id == Array.object_id, k.hash == Array.hash
p k.ah, k::X
p MyA.new.is_a?(k), k === MyA.new, (case MyA.new when k then :y else :n end)

m = [MyA.new, []][ARGV.size]
mk = m.class
p mk, mk.ma, mk.superclass, mk.superclass.ah, mk.superclass.superclass.oh
p mk.ancestors.include?(Array), mk.ancestors.index(Object), mk.ancestors.select { |c| c == Array || c == MyA }.map(&:ah)

n = [1, 2.0][ARGV.size]
p n.class.superclass, n.class.superclass.superclass.oh
p [nil, 1][ARGV.size].class == NilClass

# A class value written at compile time (`#class` of a statically typed
# receiver) is the same object as the one read off a boxed value: a native-bound
# class (StringIO, IO::Buffer), Random's, and a reopened Range's.
require "stringio"
class Range; def self.rr = :rr; end

sa = StringIO.new.class
sb = [StringIO.new, 1][ARGV.size].class
p sa.object_id == sb.object_id, sa.equal?(sb), sb.object_id == StringIO.object_id, sb
h = {}.compare_by_identity
h[sa] = 1
p h[sb], h[StringIO]
ia = IO::Buffer.new(8).class
ib = [IO::Buffer.new(8), 1][ARGV.size].class
p ia.object_id == ib.object_id, ib.name, ib.to_s, IO::Buffer.new(4).class.inspect

ra = Random.new(3).class
rb = [Random.new(1), 1][ARGV.size].class
p ra.object_id == rb.object_id, ra.equal?(rb), rb == Random, rb.name

na = (1.5..2.5).class
nb = [Object.new, (1.5..2.5)][ARGV.size + 1].class
p na.object_id == nb.object_id, na.equal?(Range), nb.rr
p ("a".."b").class.object_id == nb.object_id, (1..2).class.object_id == nb.object_id
p (Time.at(0)..Time.at(1)).class.object_id == nb.object_id
