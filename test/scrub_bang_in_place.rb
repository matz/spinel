# String#scrub! replaces the invalid bytes in the receiver itself: a later
# read of the local or the ivar sees the scrubbed text, and the call answers
# the receiver. (An alias seeing it waits for the share-by-default work.)

s = +"a\xffc"
s.scrub!("?")
p s

d = +"a\xffc"
d.scrub!
p d.bytes

class Holder
  def initialize = (@s = +"q\xffr")
  def go = (@s.scrub!("#"); @s)
end
p Holder.new.go

r = +"ok"
p r.scrub!.equal?(r)

x = +"\xff\xfe"
y = x.scrub!("")
p x, y

f = "fine".freeze
p f.scrub!
g = "a\xffb".freeze
p((g.scrub! rescue $!.class))
