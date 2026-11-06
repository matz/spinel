# Under --int-overflow=promote an Integer local is boxed, so an Array a
# method fills with one (`lo = n; t << lo`) is a poly Array, and the method's
# return widens to it once the fixpoint settles. A constant assigned from
# such a call kept the Integer Array type it was given earlier, and the
# compile refused the write into its sp_IntArray * slot. Through times, each,
# upto, each_with_index and map, push and concat, an ivar and a global, and
# with values past 64 bits that must stay Bignums.
def by_times
  t = []
  4.times { |n| lo = n; t << lo }
  t
end

def by_each
  t = []
  [5, 6, 7].each { |n| lo = n; t << lo }
  t
end

def by_upto
  t = []
  1.upto(3) { |n| lo = n; t.push(lo) }
  t
end

def by_each_with_index
  t = []
  [10, 20].each_with_index { |v, i| lo = v + 0; hi = i; t << lo; t << hi }
  t
end

def by_map
  [1, 2, 3].map { |n| lo = n; lo }
end

def by_concat
  t = []
  3.times { |n| lo = n; t.concat([lo]) }
  t
end

def by_literal
  t = []
  lo = 3
  t << lo
  t
end

def overflowing
  t = []
  3.times { |n| lo = 2**62 * (n + 1); t << lo }
  t
end

class Holder
  def fill
    @t = []
    3.times { |n| lo = n; @t << lo }
    @t
  end
end

def fill_global
  $g = []
  3.times { |n| lo = n; $g << lo }
  $g
end

TIMES = by_times
EACH = by_each
UPTO = by_upto
EWI = by_each_with_index
MAP = by_map
CONCAT = by_concat
LIT = by_literal
BIG = overflowing
IVAR = Holder.new.fill
GLOBAL = fill_global

p TIMES.fetch(3)
p EACH.fetch(2)
p UPTO
p EWI
p MAP.fetch(1)
p CONCAT
p LIT.fetch(0)
p BIG
p BIG.fetch(2).class
p IVAR.fetch(2)
p GLOBAL
p TIMES.sum + EACH.sum + UPTO.sum
