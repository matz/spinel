# A string param mutated via `<<` is the caller's String: the append lands in
# the caller's variable, like CRuby's shared-object semantics (the classic
# pass-an-output-buffer serializer pattern).
def append(s)
  s << "world"
end
str = "hello".dup
append(str)
puts str

# transitive: a wrapper that passes its param on to one that appends
def inner(s)
  s << "!"
end
def outer(s)
  inner(s)
end
a = "x".dup
outer(a)
puts a

# the mutator's return value is the receiver, usable alongside the mutation
def bang(s)
  s << "?"
end
b = "y".dup
r = bang(b)
puts b
puts r

# the sibling parameter is only read
def tag(buf, label)
  buf << "["
  buf << label
  buf << "]"
end
t = "".dup
tag(t, "hi")
puts t

# a non-lvalue argument still compiles (CRuby's mutation is equally
# invisible to the caller there)
bang("zzz".dup)
puts "lit ok"

# a plain rebind keeps value semantics: CRuby `s = ...` rebinds the local,
# invisible to the caller: a plain rebind points the local at a new String
def rebind(s)
  s = +"gone"
  s << "!"
end
u = "keep".dup
rebind(u)
puts u

# repeated appends through a call inside a block (build-a-buffer loop)
def ser(buf, n)
  buf << n.to_s
  buf << ","
end
o = "".dup
3.times { |i| ser(o, i) }
puts o

# a caller variable that is also captured by a proc (celled) passes its cell
w = "p".dup
f = proc { w << "c" }
bang(w)
f.call
puts w

# element assignment is an in-place mutator too
def fix(s)
  s[0] = "X"
end
e = "abc".dup
fix(e)
puts e

# a frozen argument raises FrozenError from inside the callee, like CRuby
frozen = "locked".freeze
begin
  bang(frozen)
rescue FrozenError
  puts "FrozenError"
end
