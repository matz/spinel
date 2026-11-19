# a class method that reads self, defined in a body, captured on a subclass
# (`S.method(:m)`: the method runs with self S), called directly, held in a local and in an
# Array (a poly slot), before and after the body runs; and a placed two-version one.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class K
  def self.setup
    def self.m(a) = [a, self]
  end
end
class S < K; end
p((S.method(:m).call(5) rescue err))
K.setup
p S.method(:m).call(5), K.method(:m).call(6)
mm = S.method(:m)
p [mm.call(7), mm.call(8)]
arr = [S.method(:m), 1]
p arr[0].call(9)
class Q
  def self.n(a) = [1, a, self]
end
class R < Q; end
if ARGV.size == 0
  class Q
    def self.n(a) = [2, a, self]
  end
end
p R.method(:n).call(3)
