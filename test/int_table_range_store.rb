# A store through a Range index splices the stored Array's elements into the
# table, so `@t[0..0] = @t[i]` stores Integers and no row: the table is no
# table of Integer rows. Taken as a move of one of its own rows, it
# narrowed, and the splice raised a TypeError at run time.
class T
  def initialize = @t = [[1, 2], [3, 4]]

  #: (Integer) -> void
  def splice(i)
    @t[0..0] = @t[i]
  end

  def first = @t[0]
end

t = T.new
t.splice(1)
p t.first
