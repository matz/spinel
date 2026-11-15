# Array.new(n, v) takes a fill that puts statements ahead of itself
# An Integer or Float fill that can be nil was emitted straight into the
# prelude, after the declaration of its temp had been written there. A fill
# that hoists statements of its own (a proc call with an argument) put them
# inside that declaration, and the C did not build.
inc = ->(q) { q + 1 }
p Array.new(2, inc.call(1))
sz = ->(q) { q.size }
s = +"ab"
p Array.new(2, sz.call(s))
p Array.new(2, sz.call([1, 2]))
p Array.new(2, sz.call("a" + "b"))
pick = ->(q) { q ? 1 : nil }
p Array.new(2, pick.call(false))
p Array.new(2, pick.call(true))
cnt = 0
tr = ->(q) { cnt += 1; q }
p Array.new(3, tr.call(7)), cnt
fl = ->(q) { q ? q * 1.5 : nil }
p Array.new(2, fl.call(2))
p Array.new(2, fl.call(nil))
n = 0
f = ARGV.size == 0
p Array.new(2, (begin; n += 1; f ? 4 : nil; rescue; nil; end)), n
