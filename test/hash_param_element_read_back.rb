# A method reads an element of a Hash parameter and returns it. The Hash the
# caller passed holds a Hash, which takes a later store with a key or a
# value of another kind.
def get(h, k) = h[k]
x = get({"a" => {:n => 1}}, "a")
x["s"] = "t"
p x

# two callers pass different Hashes
def pick(h, k)
  h[k]
end
y = pick({"a" => {:n => 1}}, "a")
z = pick({"b" => {:m => 2}}, "b")
y["s"] = "t"
z[:m] = "two"
p y, z

# the method stores through the parameter before the read
def put_get(h, k)
  h[k] = {:n => k}
  h["z"] = {:z => 1}
  h[k]
end
v = put_get({}, "a")
v["s"] = 2.5
p v

# an Array parameter's element, and a default value's
def first_of(rows) = rows[0]
r = first_of([{:n => 1}, {:n => 2}])
r["s"] = :sym
p r

def from_default(h = {"d" => {:n => 0}}, k = "d") = h[k]
d = from_default
d["s"] = "t"
p d

# a read with no later store of another kind
u = get({"a" => {:n => 1}}, "a")
p u[:n] + 1, u
