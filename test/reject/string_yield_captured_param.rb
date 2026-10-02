# A String variable on this route must not silently lose its append.
def y(v) = yield(v)
s = +"a"
y(s) { |q| f = -> { q << "!" }; f.call }
p s
