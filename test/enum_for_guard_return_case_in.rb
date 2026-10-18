# A method whose block form answers a value through a `return` inside a
# `case`/`in` clause (`return @a if @c`, a guard in the middle of the clause),
# and whose blockless call answers the Enumerator of a `return to_enum(:run)
# unless block_given?` guard. The search for a value read the guards of
# the lists the tail reaches through `if`, `unless` and parentheses, not those
# of a case/in clause, so the method was taken for an each-like one whose
# value nobody keeps: its return was pinned to the Enumerator, and the Array
# of the `return` was assigned to an sp_Enumerator in the C. The clauses of a
# case/in and its else are read now.
class Coll
  def initialize; @a = [1, 2, 3]; @k = 0; @c = true; end
  def run
    return to_enum(:run) unless block_given?
    yield(@k)
    case @k
    in 0
      return @a if @c
      [9]
    else
      [8]
    end
  end
end
c = Coll.new
p c.run { |x| x }
p c.run.class
