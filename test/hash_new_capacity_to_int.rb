# Hash.new's capacity converts an object that is not an Integer through its
# #to_int, where it is boxed too (read out of a `**h`, or out of an Array),
# and raises TypeError only where it has none or it answers no Integer.
o = Object.new
def o.to_int = 4
class Four
  def to_int = 4
end
class Word
  def to_int = "four"
end
h = { capacity: o }
p((Hash.new(**h).size rescue [$!.class, $!.message]))
p((Hash.new(capacity: [o, 1].first).size rescue [$!.class, $!.message]))
k = { capacity: Four.new }
p((Hash.new(**k).size rescue [$!.class, $!.message]))
p((Hash.new(capacity: [Four.new, 1].first).size rescue [$!.class, $!.message]))
p((Hash.new(capacity: [Object.new, 1].first).size rescue $!.class))
w = { capacity: Word.new }
p((Hash.new(**w).size rescue $!.class))
