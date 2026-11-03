class Gadget
  def secret = "s"
  def shared = "p"
  protected def guarded = "g"
  private :secret
end

class Widget
  def secret = "w"
end

[Gadget.new, Widget.new, 1].each do |c|
  p [c.respond_to?(:secret), c.respond_to?(:secret, true), c.respond_to?(:guarded), c.respond_to?(:guarded, true), c.respond_to?(:shared)]
end
