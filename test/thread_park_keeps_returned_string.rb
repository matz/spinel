# A call whose answer may be a user method's String (`src.read(n)`, where
# src is an IO or a class with its own #read) clears the runtime's
# deep-return channel, makes the call, and takes the handle the method
# published there. The channel is per OS worker: an IO#read that parked in
# between let another green thread on the worker publish its own String,
# and the reader got "other" instead of the bytes it read.
#
# One worker puts both threads on the same channel.
ENV["SPINEL_WORKERS"] = "1"

class Canned
  def initialize(s)
    @s = s
  end

  def read(n)
    @s
  end
end

class Body
  def initialize
    @raw = +""
  end

  def drain(src, n)
    chunk = src.read(n)
    return "" if chunk.nil?
    @raw = @raw + chunk
    @raw
  end
end

p Body.new.drain(Canned.new(+"canned"), 6)

r, w = IO.pipe
done = false
other = Thread.new do
  c = Canned.new(+"other")
  until done
    c.read(1)
    sleep 0.001
  end
end
reader = Thread.new { Body.new.drain(r, 5) }
sleep 0.1
w.write("hello")
p reader.value
done = true
other.join
