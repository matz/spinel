# A builtin class's method runs before a later Object def of the same name.
class TrueClass; def only(a = 1) = [:true, a]; end
class Hash; def only(a = 1) = [:hash, a]; end
class Array; def only(a = 1) = [:ary, a]; end
class FalseClass; def only(a = 1) = [:false, a]; end
p true.only(3)
p({}.only(4))
p [1].only
p false.only(5)
class Object; def only(a = 1) = [:obj, a]; end
p true.only(6)
p 3.only(7)
p "s".only
def g(v) = v.only(2)
[true, false, {}, [], 3, nil, :s].each { |v| p g(v) }

# A private override keeps its visibility.
class Hash; def pm = :hash; private :pm; end
class Array; def pq = :ary; protected :pq; end
class Object; def pm = :obj; def pq = :obj; end
p(({}.pm rescue :no_method))
p(([].pq rescue :no_method))
p 3.pm, 3.pq

# A Time or Range override answers respond_to?.
class Object; def tag(x = 1) = [:obj, x]; end
class Time; def tag(x = 1) = [:time, x]; end
class Range; def tag(x = 1) = [:range, x]; end
p Time.at(0).respond_to?(:tag), (1..2).respond_to?(:tag)
[Time.at(0), (1..2), 5, "t"].each { |v| p v.tag }

# A bare `private` after the override does not change it.
class Hash
  def late(a = 1) = [:hash, a]
  private
  def helper = 1
end
p({}.late(2))
class Object; def late(a = 1) = [:obj, a]; end
p({}.late(3))
p 3.late

# A Hash override's default reads Hash's own constant.
D = 7
class Object; def cd(x = D) = [:obj, x]; end
class Hash; D = 3; def cd(x = D) = [:hash, x]; end
p({}.cd, 5.cd)
