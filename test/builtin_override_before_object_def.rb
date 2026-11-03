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

# Each override keeps its own default value and its own body.
class Time; def tag(x = :time) = [:time, x]; end
class Range; def tag(x = :range) = [:range, x]; end
class Object; def tag(x = :obj) = [:obj, x]; end
[Time.at(0), (1..2), 5, "t"].each { |v| p v.tag }
