# A boxed value that may be ENV, read through the environment at the call:
# the receiver runs where the call does, so an `&&` that skips the call
# skips it too, and an assignment in the receiver is the one the call
# reads. clear through it empties the environment and answers ENV itself;
# clear on a Hash beside it empties that Hash.
def pick(f) = f ? ENV : nil
$n = 0
def counted(v) = ($n += 1; v)
v = pick(false)
p(v && v["SPX_A"])
w = [ENV, { "a" => 1 }]
p(w.size > 5 && counted(w[0])["SPX_A"])
p $n
ENV["SPX_A"] = "1"
p(w.size < 5 && counted(w[0])["SPX_A"])
p $n
x = nil
p((x = w[0]).fetch("SPX_A"), x.equal?(ENV))

def pick2(f) = f ? ENV : {}
h = pick2(false)
h["a"] = "b"
h.clear
p h
e = pick2(true)
ENV["SPX_Q"] = "1"
e.clear
p ENV.size, ENV.key?("SPX_Q"), e.equal?(ENV)
