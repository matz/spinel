# A boxed call that no class of its receiver answers but ENV could (a
# name ENV answers, `take_while` on a value a class with `each` may be)
# in a program where ENV is a value (#12035): the call is the raise alone,
# and it is emitted as it is, not wrapped in the read through ENV, so the
# slot coercion its callers make from its text still applies. Wrapped,
# the C did not compile (an sp_RbVal into an sp_PolyArray *).
class Rows
  def each
    yield 1
  end
end

def pick(list)
  list.take_while { |x| x < 2 }
end

def any(v) = v

p pick([1, 2, 3])
pick(Rows.new) rescue puts "NoMethodError"
any(ENV)
v = [ENV, [3, 4]][ARGV.size]
ENV["SPX_TW"] = "1"
p v.take_while { |k, _| k != "SPX_TW" }.size < ENV.size
