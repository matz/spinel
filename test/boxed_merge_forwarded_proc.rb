# A merge whose block is a proc at run time: a caller's `&pr` reaching a
# method that passes its own block on, or `&pr` written at the call. The
# forwarded block was read as no block and merged plainly, dropping it.
def mrg(h, o, &blk) = h.merge(o, &blk)
def outer(h, o, &b) = mrg(h, o, &b)
def anon(h, o, &) = h.merge(o, &)
x = [{ a: 1, b: 2 }, [1]][ARGV.size]
pr = proc { |k, o, n| [k, o, n] }
p mrg(x, { a: 10 }, &pr)
p mrg(x, { a: 10 }) { |k, o, n| o * 100 + n }
p mrg(x, { a: 10 })
p mrg(x, { a: 10 }, &nil)
p outer(x, { a: 5 }, &pr)
p anon(x, { a: 7 }, &pr)
p anon(x, { a: 7 })
p x.merge({ b: 3 }, &pr)
t = { a: 1 }
p mrg(t, { a: 2 }, &pr)
p mrg(t, { a: 2 })
p x
