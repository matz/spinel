# A row read out of a table narrowed to Integer rows, then copied into
# another local: the copy names the same row, so the pushes through it land
# in the table. Under --int-overflow=promote a computed Integer is boxed,
# and the boxed push widened the copy to the general Array; the widening
# reached the row local too, the read became a converted copy, and the
# table kept empty rows.

class Grid
  attr_accessor :t
  def initialize(n)
    @t = Array.new(n) { Array.new(0, 0) }
  end

  def fill
    k = 0
    while k < @t.length
      row = @t[k]
      al = row
      j = 0
      while j < 3
        al << k * 10 + j
        j += 1
      end
      k += 1
    end
  end

  # a copy of the copy
  def fill_again
    k = 0
    while k < @t.length
      row = @t[k]
      al = row
      al2 = al
      al2 << k * 100
      k += 1
    end
  end
end

g = Grid.new(3)
g.fill
p g.t[0][0]
p g.t[1][2]
p g.t[2][1]
p g.t[2].length
g.fill_again
p g.t[1][3]
p g.t[2][3]
p g.t[0].length
