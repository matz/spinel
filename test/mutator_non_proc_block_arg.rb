# A `&` argument that cannot become a block is CRuby's TypeError, ahead of
# the call, for a mutator written as a statement as for any other call.
# The statement forms emitted the call without the check (the Array stayed
# unchanged or took the push), and inside a block that yields a value the
# statement form was a C error.

def attempt
  yield
rescue TypeError => e
  puts e.message
end

a = [1]
attempt { a.push(2, &1); p a }
attempt { a.append(&"s"); p a }
attempt { a.concat([2], &1); p a }
attempt { a.unshift(2, &1); p a }
attempt { a.insert(0, 5, &1); p a }
h = {}
attempt { h.store(1, 2, &1); p h }
s = +"x"
attempt { s.concat("y", &1); p s }
class Foo; end
attempt { a.concat(&Foo.new); p a }

def t = yield
p((t { a.push(2, &1) } rescue $!.message))
p((t { a.push(2, &1); 5 } rescue $!.message))
x = (a.push(2, &1) rescue $!.class)
p x, a

# the receiver and the arguments run first
def r = (puts "r"; [1])
def v = (puts "v"; 2)
attempt { r.push(v, &1) }

# a value that may be none of a Proc, Method, Symbol, Hash or nil is checked
# where it runs
fs = [proc { }, 5, nil, :to_s]
fs.each { |f| attempt { a.push(7, &f) } }
p a
n = [nil, 3].first
attempt { a.push(8, &n); p a.last }
m = [nil, 3].last
attempt { a.push(9, &m); p a.last }

# unchanged: a Symbol, nil, a Proc
b = [1]
b.push(2, &:to_s)
c = nil
b.push(3, &c)
y = b.push(4, &nil)
f = proc { }
b.push(5, &f)
p b, y.equal?(b)

# a checked argument (nullable, boxed) ahead of Hash#store and String#[]=,
# which the statement form emits itself
h2 = {}
h2.store(1, 2, &n)
p h2
[proc { }, nil, :to_s].each { |f| h2.store(3, 4, &f) }
p h2
attempt { h2.store(5, 6, &m); p h2 }
s2 = +"abc"
s2.[]=(0, "Z", &n)
p s2
x2 = (s2.[]=(2, "X", &n))
p x2, s2
