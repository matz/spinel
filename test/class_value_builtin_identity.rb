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
