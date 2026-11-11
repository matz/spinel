# A `&.` call at the tail of a block whose value is read answers that value:
# the splice emitted it as the nil guard around a statement, with no value,
# and the C build stopped.

def t
  p yield
end

def maybe(i) = i.odd? ? nil : i

s = "abc"
ns = maybe(1) && "x"
t { s&.upcase }
t { ns&.upcase }
t { s&.dup }
t { ns&.dup }
t { s&.bytes }
t { ns&.bytes }
t { s&.slice(1) }
t { ns&.slice(1) }
t { s&.sub("a") { "b" } }
t { ns&.sub("a") { "b" } }
r = []
s&.each_char { |c| r << c }
p r
t { s&.each_char { |c| c } }
t { ns&.each_char { |c| c } }

t { s&.chars&.map { |c| c } }
t { ns&.chars&.map { |c| c } }

# the value is read by an assignment, an argument and a method's result
v = [1].each { s&.upcase }
p v
def tail(x) = yield(x)
p tail("q") { |x| x&.upcase }
p tail(nil) { |x| x&.upcase }
p tail("q") { |x| x&.size }
