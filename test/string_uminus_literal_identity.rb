# -x of a String with a literal's content answers that literal, as CRuby's
# fstring table holds every literal from the program's load on: the literals
# were not in the table, so -x answered a copy of its own until the literal
# itself went through -@ (#11078).
x = "ab" + "c"
p((-x).equal?("abc"))
y = "a" + "bc"
p((-y).equal?(-x), (-"abc").equal?("abc"))
def lit = "abc"
p((-x).equal?(lit), x.dup.equal?(x))
z = "q" + "rs"
p((-z).equal?(-"qrs"), z.dedup.equal?(-z))
p :abc.name.equal?(-x)
# a name kept in an instance variable is still the symbol's one String
class Held
  def initialize(v) = @v = v
  def same?(o) = @v.equal?(o)
end
p Held.new(:abc.name).same?(:abc.name), Held.new(-x).same?("abc")
