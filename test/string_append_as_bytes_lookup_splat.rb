# frozen_string_literal: true
# spinel: gc-stress
# spinel: gc-minor
class ByteCall
  def append_as_bytes(*args)
    p args
    self
  end
end
def args
  $events << 2
  [42, 43]
end
def value(n)
  $events << n
  n
end
def mixed(a)
  r = a.append_as_bytes(value(1), *args, value(3))
  p r.equal?(a)
end
$events = []
s = +"s"
mixed(s)
p s.bytes, $events
$events = []
mixed(ByteCall.new)
p $events

# The builtin and the user method both receive the String held before the
# splat changes it and assigns a different String to the same variable.
class ByteSource
  def initialize
    @part = +"x"
  end

  def spread
    @part.replace("longer")
    @part = +"different"
    [0, 88]
  end

  def write(receiver)
    receiver.append_as_bytes(@part, *spread, @part)
  end
end

s = +"!"
ByteSource.new.write(s)
p s.bytes
ByteSource.new.write(ByteCall.new)
