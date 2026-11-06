# A Method kept in an instance variable reaches its target through the Proc
# its to_proc makes, or as a block: the calls of that Proc type the target's
# parameters, so the String they pass is not read as an Integer.
class Reg
  attr_reader :rd

  def initialize
    @rd = method(:slash?)
    @pr = @rd.to_proc
    @s = +"q/r"
  end

  def slash?(k) = k.include?("/")

  def twice
    yield "a"
    yield "b/c"
  end

  def fwd(&blk) = twice(&blk)
  def via_param(m) = m.to_proc.call(@s)

  def run
    g = @rd.to_proc
    p g.call(@s)
    p g.(@s)
    p g["x"]
    p(g === "y/")
    p g.yield("z")
    p @rd.to_proc.call("1/2")
    p @pr.call(@s)
    p rd.to_proc.call("3")
    p twice(&@rd)
    p fwd(&@rd)
    p via_param(@rd)
  end
end
Reg.new.run
