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

# A top-level private def is what a send reaches on a non-owner value too.
def shout
  "loud"
end

p maybe(2).send(:shout)

# Past 64 owners every one is still split on: a non-owner value runs the
# named method, not some owner's send.
class Owner0
  def send(data)
    "owner0 #{data}"
  end
end
class Owner1
  def send(data)
    "owner1 #{data}"
  end
end
class Owner2
  def send(data)
    "owner2 #{data}"
  end
end
class Owner3
  def send(data)
    "owner3 #{data}"
  end
end
class Owner4
  def send(data)
    "owner4 #{data}"
  end
end
class Owner5
  def send(data)
    "owner5 #{data}"
  end
end
class Owner6
  def send(data)
    "owner6 #{data}"
  end
end
class Owner7
  def send(data)
    "owner7 #{data}"
  end
end
class Owner8
  def send(data)
    "owner8 #{data}"
  end
end
class Owner9
  def send(data)
    "owner9 #{data}"
  end
end
class Owner10
  def send(data)
    "owner10 #{data}"
  end
end
class Owner11
  def send(data)
    "owner11 #{data}"
  end
end
class Owner12
  def send(data)
    "owner12 #{data}"
  end
end
class Owner13
  def send(data)
    "owner13 #{data}"
  end
end
class Owner14
  def send(data)
    "owner14 #{data}"
  end
end
class Owner15
  def send(data)
    "owner15 #{data}"
  end
end
class Owner16
  def send(data)
    "owner16 #{data}"
  end
end
class Owner17
  def send(data)
    "owner17 #{data}"
  end
end
class Owner18
  def send(data)
    "owner18 #{data}"
  end
end
class Owner19
  def send(data)
    "owner19 #{data}"
  end
end
class Owner20
  def send(data)
    "owner20 #{data}"
  end
end
class Owner21
  def send(data)
    "owner21 #{data}"
  end
end
class Owner22
  def send(data)
    "owner22 #{data}"
  end
end
class Owner23
  def send(data)
    "owner23 #{data}"
  end
end
class Owner24
  def send(data)
    "owner24 #{data}"
  end
end
class Owner25
  def send(data)
    "owner25 #{data}"
  end
end
class Owner26
  def send(data)
    "owner26 #{data}"
  end
end
class Owner27
  def send(data)
    "owner27 #{data}"
  end
end
class Owner28
  def send(data)
    "owner28 #{data}"
  end
end
class Owner29
  def send(data)
    "owner29 #{data}"
  end
end
class Owner30
  def send(data)
    "owner30 #{data}"
  end
end
class Owner31
  def send(data)
    "owner31 #{data}"
  end
end
class Owner32
  def send(data)
    "owner32 #{data}"
  end
end
class Owner33
  def send(data)
    "owner33 #{data}"
  end
end
class Owner34
  def send(data)
    "owner34 #{data}"
  end
end
class Owner35
  def send(data)
    "owner35 #{data}"
  end
end
class Owner36
  def send(data)
    "owner36 #{data}"
  end
end
class Owner37
  def send(data)
    "owner37 #{data}"
  end
end
class Owner38
  def send(data)
    "owner38 #{data}"
  end
end
class Owner39
  def send(data)
    "owner39 #{data}"
  end
end
class Owner40
  def send(data)
    "owner40 #{data}"
  end
end
class Owner41
  def send(data)
    "owner41 #{data}"
  end
end
class Owner42
  def send(data)
    "owner42 #{data}"
  end
end
class Owner43
  def send(data)
    "owner43 #{data}"
  end
end
class Owner44
  def send(data)
    "owner44 #{data}"
  end
end
class Owner45
  def send(data)
    "owner45 #{data}"
  end
end
class Owner46
  def send(data)
    "owner46 #{data}"
  end
end
class Owner47
  def send(data)
    "owner47 #{data}"
  end
end
class Owner48
  def send(data)
    "owner48 #{data}"
  end
end
class Owner49
  def send(data)
    "owner49 #{data}"
  end
end
class Owner50
  def send(data)
    "owner50 #{data}"
  end
end
class Owner51
  def send(data)
    "owner51 #{data}"
  end
end
class Owner52
  def send(data)
    "owner52 #{data}"
  end
end
class Owner53
  def send(data)
    "owner53 #{data}"
  end
end
class Owner54
  def send(data)
    "owner54 #{data}"
  end
end
class Owner55
  def send(data)
    "owner55 #{data}"
  end
end
class Owner56
  def send(data)
    "owner56 #{data}"
  end
end
class Owner57
  def send(data)
    "owner57 #{data}"
  end
end
class Owner58
  def send(data)
    "owner58 #{data}"
  end
end
class Owner59
  def send(data)
    "owner59 #{data}"
  end
end
class Owner60
  def send(data)
    "owner60 #{data}"
  end
end
class Owner61
  def send(data)
    "owner61 #{data}"
  end
end
class Owner62
  def send(data)
    "owner62 #{data}"
  end
end
class Owner63
  def send(data)
    "owner63 #{data}"
  end
end
class Owner64
  def send(data)
    "owner64 #{data}"
  end
end
class Owner65
  def send(data)
    "owner65 #{data}"
  end
end

def among(i)
  i == 0 ? Owner65.new : Time.at(0).utc
end

p among(0).send("year")
p among(1).send("year")
