# `raise error, "message"` with an error object built in the expression: the
# object is held while the message and the raised copy are made, so it keeps
# its name, arguments and receiver (also under GC stress)
class W
  def kw(a)
    raise NoMethodError.new("plain", :baz, [a], receiver: self), "override"
  rescue NoMethodError => e
    [e.message, e.name, e.args, e.receiver.equal?(self)]
  end
  def pos(a)
    raise NoMethodError.new("plain", :baz, [a]), "override"
  rescue NoMethodError => e
    [e.message, e.name, e.args]
  end
end
def name_error(x)
  raise NameError.new("plain", :zz, receiver: x), "over"
rescue NameError => e
  [e.message, e.name, e.receiver]
end
w = W.new
junk = (0..20).map { |i| ["s#{i}", i] }
p junk.size
p w.kw(7), w.kw(8)
p w.pos(7), w.pos(8)
r = (0..20).map { |i| name_error("r#{i}" * 2) }
p r[0], r[20]
