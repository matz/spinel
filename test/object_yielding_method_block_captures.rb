# A yielding method of Object's (a reopening's, or a module's Object
# includes), called in a class's method with a block that reads or writes a
# local of that method: the block is a real proc there, and its captures
# are kept.
class Object
  def twice = yield + yield
end
module Runs
  def thrice = yield + yield + yield
end
Object.include(Runs)

class Plain
  def sum(x) = twice { x }
  def sum2(x) = self.twice { x + 1 }
  def other(x) = Plain.new.twice { x * 2 }
  def nest = [3].map { |x| twice { x } }
  def acc
    n = 0
    twice { n += 1 }
    n
  end
  def three(s) = thrice { s }
end
p Plain.new.sum(4)
p Plain.new.sum2(4)
p Plain.new.other(4)
p Plain.new.nest
p Plain.new.acc
p Plain.new.three("ab")
y = 5
p twice { y }
p 3.thrice { y }
