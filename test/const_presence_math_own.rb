# A Math function the program defines is the program's code: a constant
# computed with it may run a read of another constant before that one is
# assigned, which raises NameError.
class Holder
  def self.k; K; end
end
module Math
  def self.cbrt(x)
    Holder.k
  rescue NameError => e
    puts e.message
    x
  end
end
C3 = Math.cbrt(27.0)
class Holder
  K = 3
end
p C3
p Holder.k
