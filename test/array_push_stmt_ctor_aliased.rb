# A statement Array push runs its receiver before an argument that runs the constructor again
# `@reel << @hand.pull` keeps its one C call while only the constructor
# assigns @reel, which holds as long as the constructor runs once. Under an
# alias it runs again, so a program that names it reads the receiver first.
class Hand
  def initialize(owner) = @owner = owner
  def pull = (@owner.reset; 5)
end

class Reel
  def initialize
    @reel = [1]
    @hand ||= Hand.new(self)
  end
  alias reset initialize
  public :reset

  def run
    old = @reel
    @reel << @hand.pull
    p old, @reel
  end
end
Reel.new.run
