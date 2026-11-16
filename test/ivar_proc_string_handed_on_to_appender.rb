# A proc that hands its String parameter on to a method that hands it on to
# one that appends, called with a String instance variable: the ivar's slot
# is the shared handle, so the append reaches it.
class Buffer
  def initialize = (@s = +"x")
  def add(s) = (s << "!")
  def relay(s) = add(s)
  def run
    f = ->(s) { relay(s) }
    f.call(@s)
    p @s
  end
end
Buffer.new.run
