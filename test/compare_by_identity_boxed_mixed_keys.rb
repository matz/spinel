# An identity Hash whose keys come boxed through containers and objects, of
# several kinds (#11078: eggc's `{}.compare_by_identity` keyed by Struct
# values was refused when the analysis could not show what the key held).
Sig = Struct.new(:name, :params)
class Checker
  attr_reader :concrete
  def initialize
    @queue = []
    @concrete = []
  end
  def enqueue(sig) = @queue << sig
  def check
    until @queue.empty?
      sig = @queue.shift
      @concrete << sig
    end
  end
end
c = Checker.new
a = Sig.new("f", ["i64"])
c.enqueue(a)
c.enqueue(Sig.new("f", ["i64"]))   # equal to a, another object
c.enqueue(a)
c.check
h = {}.compare_by_identity
c.concrete.each { |sig| (h[sig] ||= []) << sig.name }
p h.size, h[a], h.key?(Sig.new("f", ["i64"]))
mixed = [:k, "s", 7, a, [1], nil, true]
g = {}.compare_by_identity
mixed.each_with_index { |k, i| g[k] = i }
p g.size, g[:k], g[7], g[a], g[nil], g[true], g["s".dup]
