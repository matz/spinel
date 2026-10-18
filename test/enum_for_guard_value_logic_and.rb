# A method whose block form answers `false` through the left side of an `&&`
# (`@f && @a.each { ... }`), and whose blockless call answers the Enumerator of
# a `return to_enum(:run) unless block_given?` guard. The search for a value
# in the method's tail read the right side of an `&&` only, so the method was
# taken for an each-like one: its return was pinned to the Enumerator, and the
# block form answered nil where CRuby answers false. The left side is a value
# position too.
class Coll
  def initialize; @a = [1, 2, 3]; @f = false; end
  def run
    return to_enum(:run) unless block_given?
    @f && @a.each { |x| yield x }
  end
end
c = Coll.new
p c.run { |x| x }
p c.run.class
