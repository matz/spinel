# A `private def` or `protected def` in a `class << obj` body defines a
# method of that one object, with that visibility. Another object of the
# class, or any other object, does not get it.
class W; def inspect = "#<W>"; end
v = W.new
class << v
  private def hid = :h
  protected def pro = :p
  if true
    private def hid2 = :h2
  end
  def go = [hid, pro, self.pro, hid2]
end
p v.go
p v.send(:hid)
p((v.hid rescue "NoMethodError"))
p((v.pro rescue "NoMethodError"))
p((hid rescue "NameError"))
p((W.new.send(:hid) rescue "NoMethodError"))
p((Object.new.send(:hid2) rescue "NoMethodError"))
p v.respond_to?(:hid), v.respond_to?(:hid, true), v.respond_to?(:pro)
p W.new.respond_to?(:hid, true), W.new.respond_to?(:pro, true)
p Object.private_method_defined?(:hid), W.method_defined?(:pro)

# an instance variable's object, and a private def over a top-level def
class Box
  def initialize
    @o = W.new
    class << @o
      private def sec = 42
      def show = sec
    end
  end
  def run = [@o.show, (@o.sec rescue "NoMethodError")]
end
p Box.new.run
p((Object.new.send(:sec) rescue "NoMethodError"))
def helper = :top
class << v
  private def helper = :own
  def call_helper = helper
end
p v.call_helper
p W.new.send(:helper)

# an implicit Array conversion takes the object's private to_ary
u = W.new
class << u
  private def to_ary = [:s]
end
p [1] + u
p W.new.respond_to?(:to_ary, true)
