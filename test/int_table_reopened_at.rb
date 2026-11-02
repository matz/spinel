# A read out of a table is one of its own rows only through Array's own
# `at` and `[]`. A program that reopens `Array#at` gets its answer, so
# `@t[i] = @t.at(j)` stores a String here, and the table must stay boxed:
# taken as a row of the table, the String was read back as an Integer row.
class Array
  def at(i) = "row"
end

class T
  def initialize = @t = [[1, 2], [3, 4]]

  #: (Integer, Integer) -> void
  def move(i, j)
    @t[i] = @t.at(j)
  end

  def first = @t[0]
end

t = T.new
t.move(0, 1)
p t.first
