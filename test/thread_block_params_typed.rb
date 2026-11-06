# spinel: int64
# A Thread.new block takes Thread.new's arguments. They cross the runtime
# boundary boxed, and the block's parameters stayed boxed whatever they
# were given, so a worker loop over its index ran on boxed values. An
# Integer or a Float parameter is now typed from the argument and taken out
# of the box on entry; any other kind, or a parameter of another form,
# stays boxed.

# the worker loop over its index
acc = Array.new(8, 0)
ws = []
w = 0
while w < 4
  ws << Thread.new(w) do |wid; i|
    i = wid
    while i < 8
      acc[i] = i * 10 + wid
      i += 4
    end
  end
  w += 1
end
ws.each(&:join)
p acc

# several parameters, one per argument
p Thread.new(3, 9) { |lo, hi| (lo..hi).sum }.value

# a Float
p Thread.new(1.5) { |f| f * 2 }.value

# a String stays boxed
p Thread.new("ab") { |s| s * 2 }.value

# the same block spawned with an Integer and a String
def spawn(v) = Thread.new(v) { |x| x.to_s * 2 }
p spawn(4).value
p spawn("z").value

# a parameter the body writes a String to
p Thread.new(1) { |x| x = "s#{x}"; x }.value

# no argument, and a nil one
p Thread.new { |x| x.inspect }.value
p Thread.new(nil) { |x| x.nil? }.value

# an argument that may be nil: the parameter holds it
def maybe_int(flag) = Thread.new(flag ? nil : 1) { |x| x.nil? ? :none : x + 1 }.value
p maybe_int(true)
p maybe_int(false)
def maybe_float(flag) = Thread.new(flag ? nil : 1.5) { |x| x.inspect }.value
p maybe_float(true)
p maybe_float(false)

# a receiver written from the root
p ::Thread.new(1) { |x| x + 1 }.value

# a parameter a lambda in the body captures and changes
p Thread.new(5) { |x| pr = -> { x += 1 }; pr.call; pr.call; x }.value

# more arguments than parameters, and fewer
p Thread.new(1, 2) { |a| a }.value
p Thread.new(1) { |a, b| [a, b] }.value

# an Integer near the limit, multiplied in the body
big = 2**61
p Thread.new(big) { |x| x * 2 }.value
