# A module's own `def method` with `super`: in a class that includes it,
# the super is Object#method, as a class's own one is (#10719). The
# counting override runs once per lookup, for each class that includes it,
# and for a bare super and one with its own argument alike.
$n = 0
module Hook
  def method(n)
    $n += 1
    n == :other ? super(:keep) : super
  end
end
class H; include Hook; def keep(x) = x * 4; end
class G
  include Hook
  def keep(x) = x + 1
end
p H.new.method(:keep).call(2), $n
p G.new.method(:keep).call(2), $n
p G.new.method(:other).call(5), $n
p [1, 2].map(&H.new.method(:keep)), $n
