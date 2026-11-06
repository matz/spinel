# `io.read(*args)` on a value that may be a File or an object of the
# program's own class with a `read` (WEBrick's `io.__send__(method, *arg)`
# reading a request body): the File reads itself, as without the splat.

require "stringio"

class Chunk
  def initialize(s) = (@s = s)
  def read(*a) = "chunk #{@s} #{a.inspect}"
end

def rd(io, *arg) = io.read(*arg)

f = File.open(__FILE__)
p rd(Chunk.new(1), 3)
p rd(StringIO.new("abcdef"), 2)
p rd(f, 5)
p rd(f, nil).size > 0
p rd(f)
p rd(f, 4)
f.close
