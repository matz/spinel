# Direct calls and fresh splatted keyword values retain their behavior.
def m(k1:) = k1 << "x"
d = +"d"; m(**{ k1: d }); p d
p m(**{ k1: +"c" })
q = method(:m)
p q.call(**{ k1: +"e" }, **{})

# A later explicit keyword replaces the variable in the splatted Hash.
def append_key(k:) = k << "x"
s = +"s"
p method(:append_key).call(**{ k: s }, k: +"other")
p s
def yield_key_proc(s, &b) = yield(**{ k: s }, k: +"other")
p yield_key_proc(s, &method(:append_key))
p s
def yield_key_block(s) = yield(**{ k: s }, k: +"other")
p yield_key_block(s) { |k:| k << "x" }
p s
