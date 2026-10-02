# A container a proc or a lambda is called with is the one its body
# stores into, as in CRuby: a push or a key the body writes through a
# boxed parameter widens the Array or Hash the call hands it, directly or
# through a method that calls the proc with its own parameter. The typed
# container kept its kind, and the store raised TypeError at run time.

l = ->(a) { a.push(1.5); a }; p l.call(["s"])
h = {}; pr = proc { |t| t[:k] = 2 }; pr.call(h); p h
def fw(f, x) = f.call(x)
pr = proc { |t| t[:k] = 2 }
h = {}; fw(pr, h); p h
l = ->(a) { a << :s }
xs = [1, 2]; l.call(xs); p xs
def fw2(f, x) = (f.(x); x)
p fw2(->(a) { a.push("z") }, [1])
g = {}; ->(t) { t[1] = "one" }.call(g); p g
def fw3(f, x) = f.call(x)
l = ->(a) { a.push(1.5); a }
p fw3(l, ["s"])
