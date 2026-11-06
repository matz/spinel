# Data#with on a boxed receiver keeps an Integer member typed, with its
# nil bit, when every value it can take is an Integer or nil: a nil that
# a typed String-to-Integer Hash holds reaches the member through `**`,
# when that Hash is the member's only source of nil.
Nt = Data.define(:a)
Nq = Data.define(:a)

def make_n(flag)
  flag ? Nt.new(a: 1) : Nq.new(a: 2)
end

def put_val(hh, key, val)
  hh[key] = val
  nil
end

vals = {"a" => 1}
put_val(vals, "a", 3)
put_val(vals, "a", nil)
p vals
p make_n(true).with(**vals)
p make_n(false).with(**vals)
nv = make_n(true).with(**vals)
p nv.a.nil?
p nv.with(a: 7)
p make_n(false).with(**vals).with(a: 9)

cnt = {"a" => 4}
p make_n(true).with(**cnt)
