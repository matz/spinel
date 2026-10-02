# StringIO is a class the stringio package binds to C: a subclass of it has
# none of its methods (#7075).
require "stringio"

class Buffer < StringIO
end

b = Buffer.new
b.write("ab")
p b.string
