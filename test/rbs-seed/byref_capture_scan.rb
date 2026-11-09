# A String parameter that a lifted proc also captures. The capture
# struct once held the parameter's CELL, and for a lent parameter that cell was
# the CALLER's slot -- here the address of a stack local -- not a GC object.
# The capture's scan marked it anyway, so a collection that reached this proc
# read a header off the stack and called through it: SIGSEGV in the mark, with
# the frame naming a view method (#4391, rubys, on a campfire port where 140
# of 144 view methods take the ABI).
#
# Three things have to line up, which is why it took a real server to arrive:
# the `--rbs` seed keeps `io` a String so it was lent even though
# it is captured; `items` is untyped, so `each` cannot resolve statically and
# the block is lifted to a real proc rather than inlined; and a collection has
# to run while that proc is live. Under SPINEL_GC_STRESS=1 the last one is
# every allocation, which is what makes this deterministic here.
class Bag
  def initialize(a); @a = a; end
  def each(&blk)
    @a.each { |x| blk.call(x) }
    self
  end
end

module V
  def self.one_into(io, x)
    io << "<i>#{x}</i>"
    nil
  end

  def self.all_into(io, items)
    items.each { |x| V.one_into(io, x) }
    io << "\n"
    nil
  end

  def self.all(items)
    io = String.new
    V.all_into(io, items)
    io
  end
end

def rows(f, n)
  a = []
  n.times { |i| a << "r#{i}" }
  f ? Bag.new(a) : a
end

total = 0
40.times { |k| total += V.all(rows(k.even?, 20)).length }
puts total
