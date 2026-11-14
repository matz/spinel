# String#getbyte on a mutable String with an index that is arithmetic: the
# index builds nothing, so the receiver is read where the call needs it --
# in a loop that holds the String's bytes, only in the out-of-range arm --
# and not fetched and rooted ahead of the index on every iteration. An
# index that does build something runs first, into a temp, and the
# receiver is read in the call after it.
Thread.new { 0 }.join

class Ctx
  attr_reader :rows
  def initialize(rows)
    @rows = rows
    @rows.setbyte(0, 7) # written in place: a mutable String
  end
end

# in range, rows of p bytes
def total(ctx, n, p)
  rows = ctx.rows
  s = 0
  j = 0
  while j < n
    k = 0
    while k < p
      s += rows.getbyte(j * p + k)
      k += 1
    end
    j += 1
  end
  s
end

# the inner index runs from before the start to past the end: negative
# indexes count from the end, and past the end is nil
def edges(ctx, n, p)
  rows = ctx.rows
  s = 0
  nils = 0
  j = 0
  while j < n
    k = -p
    while k < p
      b = rows.getbyte(j * p * 2 + k)
      if b
        s += b
      else
        nils += 1
      end
      k += 1
    end
    j += 1
  end
  [s, nils]
end

# a loop that calls a method caches nothing: the index is still arithmetic
def bump(x)
  x + 1
end

def uncached(ctx, n)
  rows = ctx.rows
  s = 0
  i = 0
  while i < n
    s += rows.getbyte(i * 2) || 0
    i = bump(i)
  end
  s
end

# an index that allocates (an Array literal) runs ahead of the receiver's read
def built(ctx, n)
  rows = ctx.rows
  s = 0
  i = 0
  while i < n
    s += rows.getbyte([i, 3].sum) || 0
    i += 1
  end
  s
end

str = +""
40.times { |i| str << (i * 5 + 1).chr }
ctx = Ctx.new(str)
p total(ctx, 8, 5)
p edges(ctx, 3, 9)
p uncached(ctx, 25)
p built(ctx, 40)
