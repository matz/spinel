# A Hash stored into another Hash by `[]=` and read back by `[]` takes a
# later store with a key or a value of another kind.
root = {}
root["a"] = {:name => "a"}
a = root["a"]
a["b"] = 1
p root, a

# a Symbol key, and a key read from a local
tbl = {}
tbl[:x] = {"n" => 1}
k = "y"
tbl[k] = {"n" => 2}
x = tbl[:x]
x[:sym] = "s"
y = tbl[k]
y["n"] = "two"
p tbl

# the literal's own value and a stored one
both = {:first => {:n => 1}}
both[:second] = {:n => 2}
s = both[:second]
s["str"] = 2.5
f = both[:first]
f["str"] = :sym
p both

