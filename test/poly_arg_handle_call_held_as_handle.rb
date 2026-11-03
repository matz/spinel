# A String argument a call answers as its handle is held as the handle
# One arm of a dispatch over the receiver's class takes a String parameter
# that holds the shared handle, so the argument `fmt` (a method answering
# `+"C"`) is read as its handle. The dispatch held every argument at its
# type's C form, the bytes, and initialized that temp with the handle: the
# C did not compile. The temp now holds what the read yields.
class Rest
  def pack(*args, **kw) = "rest:#{args}:#{kw}"
end
class Named
  def pack(fmt, buffer: nil) = "named:#{fmt}:#{buffer.inspect}"
end
def fmt
  +"C"
end
[[65], Rest.new, Named.new].each do |a|
  b = +"b"
  p a.pack(fmt, buffer: b), b
end
[[65], Rest.new, Named.new].each do |a|
  p a.pack("C", buffer: nil)
end

# a native arm beside them: StringIO#putc takes the handle as the String it
# is (its [:string] binding), not as its [:int] one
require "stringio"
class Rest
  def putc(*args, **kw) = "rest:#{args}:#{kw}"
end
class Named
  def putc(s, buffer: nil) = "named:#{s}:#{buffer.inspect}"
end
io = StringIO.new
[io, Rest.new, Named.new].each { |a| p a.putc(fmt) }
p io.string
