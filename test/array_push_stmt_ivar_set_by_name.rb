# A statement Array push runs its receiver before an argument that sets it by name
# `@reel << @hand.pull` keeps its one C call while only the constructor
# assigns @reel. instance_variable_set assigns it with no write the compiler
# sees, so a program that calls it reads the receiver first.
class Hand
  def initialize(owner) = @owner = owner
  def pull = (@owner.instance_variable_set(:@reel, [100]); 5)
end

class Reel
  def initialize
    @reel = [1]
    @hand = Hand.new(self)
  end

  def run
    old = @reel
    @reel << @hand.pull
    p old, @reel
  end
end
Reel.new.run
