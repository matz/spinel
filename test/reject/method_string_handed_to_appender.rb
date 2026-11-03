# A Method of a method that appends to its parameter, read out of an
# instance variable and called through its proc with a String the call
# cannot share (an instance variable): the append would be lost, so it is
# refused.
# spinel: reject-subclass: a proc or Method it can reach appends
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
