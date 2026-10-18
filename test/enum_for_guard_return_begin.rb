# A method whose block form answers a value through a `return` inside a
# `begin` (`return @a if @c`, a guard in the middle of its body), and whose
# blockless call answers the Enumerator of a `return to_enum(:run) unless
# block_given?` guard. The search for a value read the guards of the lists the
# tail reaches through `if`, `unless` and parentheses, not those of a begin,
# so the method was taken for an each-like one whose value nobody keeps: its
# return was pinned to the Enumerator, and the Array of the `return` was
# assigned to an sp_Enumerator in the C. The body, the rescue clauses and the
# else clause of a begin are read now.
class Coll
  def initialize; @a = [1, 2, 3]; @c = true; end
  def run
    return to_enum(:run) unless block_given?
    begin
      return @a if @c
      yield(0)
    rescue ArgumentError
      nil
    end
  end
end
c = Coll.new
p c.run { |x| x }
p c.run.class
