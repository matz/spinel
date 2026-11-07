# Hash#compare_by_identity keyed by a value read out of a Hash used as a
# record: `h[:sig]` holds what was stored under :sig, not the String or the
# Integer under its other keys. A `new` on a class value the walk does not
# know reaches no initialize it passes too many arguments for.
Sig = Struct.new(:name)

class Node
  attr_accessor :info
end

def typed(sig)
  n = Node.new
  n.info = {}
  n.info[:sig] = sig
  n.info[:type] = "str"
  n.info[:len] = 3
  n
end

def literal(sig)
  n = Node.new
  n.info = { sig: sig, local: true, len: 4, name: "x" }
  n
end

class Summary
  def initialize = @seen = {}.compare_by_identity

  def add(info)
    callee = info&.[](:sig)
    return if callee.nil?
    @seen[callee] ||= {}
    @seen[callee][:types] ||= []
    @seen[callee][:types] << info[:type].to_s
  end

  def types(sig) = @seen.dig(sig, :types)
  def size = @seen.size
end

a = Sig.new("a")
b = Sig.new("a")
s = Summary.new
[typed(a), typed(a), literal(b)].each { |n| s.add(n.info) }
p s.size
p s.types(a)
p s.types(b)
p s.types(Sig.new("a"))

class Holder
  attr_reader :v
  def initialize(v) = @v = v
end

class Triple
  attr_reader :all
  def initialize(x, y, z) = @all = [x, y, z]
end

k = [Triple, Triple][ARGV.size]
p k.new("x", 1, 2.5).all
h = {}.compare_by_identity
h[Holder.new(a).v] = 1
h[a] = 2
p h.size, h[a]
