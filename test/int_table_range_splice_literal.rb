# A store through a Range index splices the stored Array's elements into the
# table: `@t[0..0] = [5, 6]` stores the Integers 5 and 6 and no row, so the
# table is no table of Integer rows. The value is an Integer Array, which
# narrow_int_table_ivars took for a row, and the splice raised a TypeError at
# run time. A Range held in a parameter is the same splice; a store through
# an Integer index still narrows (W).
class T
  def initialize = @t = [[1, 2], [3, 4]]

  def splice
    @t[0..0] = [5, 6]
  end

  def first = @t[0]
end

class R
  def initialize = @t = [[1, 2], [3, 4], [7, 8]]

  def splice(r)
    @t[r] = [9]
  end

  def second = @t[1]
end

class W
  def initialize = @t = [[1, 2], [3, 4]]

  def put
    @t[1] = [5, 6]
  end

  def sum = @t[0][0] + @t[1][1]
end

t = T.new
t.splice
p t.first
r = R.new
r.splice(1..2)
p r.second
w = W.new
w.put
p w.sum
