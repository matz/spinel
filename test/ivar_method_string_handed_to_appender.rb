# A Method of a method that appends to its parameter, read out of an
# instance variable and called through its proc with a String instance
# variable: the ivar's slot is the shared handle, so the append reaches it.
class Buffer
  def initialize
    @add = method(:add)
    @s = +"x"
  end
  def add(k) = (k << "!"; true)
  def run
    p [+"a", +"b"].select(&method(:add))
    g = @add.to_proc
    g.call(@s)
    p @s
  end
end
Buffer.new.run
