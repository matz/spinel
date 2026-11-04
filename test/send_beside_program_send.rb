# A literal-name send on a boxed value, in a program where a class defines
# its own `send` (a socket's `send(data)`): only that class's values take
# it; a Time or a String in the same slot runs the method the name names,
# as Object#send does. Kept whole, the class's send took every value and
# typed `to_time.send("year")` as its answer (et-orbi beside a WebSocket).
module Net2
  class Sock
    def send(data)
      "sent #{data}"
    end
  end
end

def pick(i)
  i == 0 ? Net2::Sock.new : (i == 1 ? Time.at(0).utc : "abc")
end

class Wrap
  def initialize(k)
    @k = k
  end

  def to_time
    @t ||= (@k > 0 ? Time.at(0).utc : nil)
  end

  def year
    to_time.send("year")
  end
end

p pick(0).send("year")
p pick(1).send("year")
p pick(2).send(:upcase)
p pick(1).send(:strftime, "%Y")
p Wrap.new(1).year - 1969
p Net2::Sock.new.send(:x)

# `&.` keeps its nil answer on the non-owner arm, and public_send there
# still refuses a private method.
class Quiet
  def initialize(v)
    @v = v
  end

  private

  def secret
    @v
  end
end

def maybe(i)
  i == 0 ? Net2::Sock.new : (i == 1 ? nil : Quiet.new(4))
end

p maybe(1)&.send(:year)
p maybe(0)&.send(:year)
p maybe(2).send(:secret)
begin
  maybe(2).public_send(:secret)
rescue NoMethodError => e
  p e.class
end
