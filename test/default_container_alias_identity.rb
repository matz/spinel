# spinel: share
# Default parameters aliased to another parameter or local widen the source in
# place so both names keep the same container identity and share mutations.

def append_default(a, b = a)
  b << "x"
  b.equal?(a)
end

arr = [1]
eq_arr = append_default(arr)
p arr
p eq_arr
p arr.length

def set_default(a, b = a)
  b[1] = "z"
  b.equal?(a)
end

h = { "k" => "v" }
eq_h = set_default(h)
p h
p eq_h

def cleared_local(params: (loc = { "" => "" }; loc.clear; loc))
  params["p"] = "v"
  [loc, params.equal?(loc)]
end

p cleared_local
p cleared_local(params: { "q" => "ok" })
