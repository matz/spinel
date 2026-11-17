# spinel: gc-stress
class InterpLabel
  def initialize(text) = @text = text
  def to_s = @text
end
class InterpSub < String
end
def shorten(s)
  s.chop!
  "!"
end
s = +"frog"
label = InterpLabel.new(s)
p "#{label}#{shorten(s)}".bytes
s = +"a\0z"
label = InterpLabel.new(s)
p "#{label}" "#{shorten(s)}".bytes
s = InterpSub.new("abcd")
p "#{s}#{shorten(s)}".bytes
s = +"reed"
xs = [InterpLabel.new(s), 5]
p "#{xs[0]}#{shorten(s)}".bytes
s = +"pond"
xs = [s, 3]
p "#{xs[0]}#{shorten(s)}".bytes
