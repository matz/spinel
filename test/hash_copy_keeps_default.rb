# A Hash copied into a slot of another variant keeps its default.
#
# The variants are separate C structs, so a hash entering a slot of another
# variant is copied entry by entry (#4089). The copy had no default: a
# `Hash.new(0)` built as a String-keyed Integer hash and widened into a local
# that a block over an unresolved receiver writes a poly key into answered nil
# for a new key, and `+= 1` on it raised. A default block fared the same: a
# `Hash.new { |h, k| h[k] = [] }` held in a slot the program types
# Hash[Symbol, untyped] answered nil to the first read of every key -- the
# load-hook table of a large application program, read at its first hook.

class Pairs
  def each
    yield "a", 1
  end
end

module Cnt
  def self.counts
    h = Hash.new(0)
    h["seed"] += 1
    h
  end
end

def tally(flat)
  sub = Cnt.counts
  flat.each { |k, v| sub[k] += v }
  sub["new"] += 1
  sub
end

p tally({ "a" => 1 })
p tally(Pairs.new)

module Hooks
  #: () -> Hash[Symbol, untyped]
  def self.table = Hash.new { |h, k| h[k] = [] }
end

t = Hooks.table
t[:boot] << "a"
t[:boot] << "b"
p t[:boot]
p t[:other]
p t
