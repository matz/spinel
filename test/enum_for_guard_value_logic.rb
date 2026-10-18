# A method whose block form answers a value through an `||` (`yield(@k) || @a`), and whose
# blockless call answers the Enumerator of a `return to_enum(:run) unless
# block_given?` guard. The search for a value in the method's tail did not
# read this shape, so the method was taken for an each-like one whose value
# nobody keeps: its return was pinned to the Enumerator, and the Array the
# block form answers came back as the Enumerator or stopped the build. The
# shape is read now, and the method answers the Array.
class Coll
  def initialize; @a = [1, 2, 3]; @k = 99; end
  def run
    return to_enum(:run) unless block_given?
    yield(@k) || @a
  end
end

c = Coll.new
p c.run { |x| nil }
p c.run.class
