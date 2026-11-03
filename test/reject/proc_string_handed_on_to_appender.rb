# A proc that hands its String parameter on to a method that hands it on to
# one that appends: a String the call cannot share (an instance variable)
# would go over as a copy and the append would be lost, so it is refused.
# spinel: reject-subclass: which the proc appends to
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
