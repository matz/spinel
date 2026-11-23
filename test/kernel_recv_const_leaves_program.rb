# `Kernel.puts x` becomes the bare `puts x`: its receiver is dropped. The
# constant stayed in the node table, no tree's, and read as a use of Kernel
# as a value. A boxed receiver's `io.gets` then got an arm for `Kernel.gets`
# (as a boxed Time's `k.now` gets one for `Time.now`), and that arm was
# refused where the program has a `gets` of its own: "unsupported call:
# CallNode `gets`", for a program that writes no `Kernel.gets`.

class Reader
  def gets
    "own"
  end
end

def note(message)
  Kernel.puts(message)
end

def first_line(io)
  io.gets
end

note("start")

# out of an Array: a boxed receiver
box = [File.open(__FILE__), Reader.new]
f = box[0]
puts f.gets[0, 16]
r = box[1]
puts r.gets
f.close

# as a parameter: boxed, with the two classes at its call sites
g = File.open(__FILE__)
puts first_line(g)[0, 16]
puts first_line(Reader.new)
g.close

# through Thread.new's argument, which the block takes boxed
h = File.open(__FILE__)
t = Thread.new(h) do |io|
  first_line(io)[0, 16]
end
puts t.value
h.close
