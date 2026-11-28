# A Hash made by `Hash.new(default).tap { }`: the tap is lowered to a write of
# the receiver and the block's statements, and the Hash's variant, pinned on
# the tap, is the write's; the constructor takes it from there (it raised
# "undefined method 'new' for class Hash" at run time).
h = Hash.new(0).tap { }
h["b"] = 1
p h
g = Hash.new(0).tap { |t| t["a"] += 1 }
p g, g["x"]
e = Hash.new.tap { |t| t.store("k", :v) }
p e["k"], e.size
