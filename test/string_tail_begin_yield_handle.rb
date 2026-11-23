# frozen_string_literal: true
# A String answered at the tail of a begin/rescue, of a begin body stored
# into an Array, or by a block a method yields to is the String itself:
# the exception's own message, an element of an Array, a held String. The
# caller mutates what it was handed and prints the holder.

# a method whose tail is a begin/raise/rescue answering e.message
def guarded(n)
  raise ArgumentError, "boom #{n}" if n > 0
  +"calm"
rescue ArgumentError => e
  e.message
end
m = guarded(1)
puts m, m.frozen?, m.equal?(guarded(1)), guarded(0)

# the message of an exception a program holds
$exc = (begin; raise +"emsg"; rescue => e; e; end)
def msg = $exc.message
r = msg
puts r, r.equal?($exc.message), r.frozen?

# a begin body answering an element, stored into an Array
w = [+"w0", +"w1"]
b = [nil, 1]
b[0] = begin
  q = [w[0]]
  q[0]
end
b[0] << "!"
p b, w

# a yielded element, through a begin and through a method on it
def it
  c = [nil, 1]
  c[0] = begin
    q = [yield]
    q[0]
  end
  c[0]
end
x = it { w[0] }
x << "?"
p x, w, x.equal?(w[0])

def each_size = yield.size
p each_size { w[0] }, each_size { w[1] }, each_size { +"abc" }
w[0] << "#"
p each_size { w[0] }

# an exception's message answered through a yield
y = it { $exc.message }
p y, y.equal?($exc.message)
