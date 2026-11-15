# A method a reopening of String adds runs with the String subclass
# instance itself as self, as in CRuby: equal? holds, an in-place change
# and an ivar the method keeps are the instance's, and a builtin call on
# self still answers a new String. The method was handed a copy of the
# instance's bytes, so self was another String, and an ivar it kept was
# lost.
class String
  def me = self
  def shout = upcase + "!"
  def grow! = (self << "+"; self)
  def note!(v)
    @note = v
    self
  end
  def note = @note
end
class Text < String; end
t = Text.new("ab")
p t.me.equal?(t)
p t.shout, t.shout.class
p t.to_s.equal?(t), t.to_s.class, t.upcase.class
t.grow!.grow!
p t, t.class
p t.note!(:n).note, t.note
u = Text.new("cd")
p u.note
