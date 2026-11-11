# spinel: not-cruby -- under --no-bigint the overflow raises where CRuby grows a Bignum
# --no-bigint: the fibonacci accumulators the loop-growth detector would
# widen to Bignum stay Integer; a value in range answers as usual, and the
# overflow raises as any raise-mode Integer arithmetic does.
def fib(n)
  a = 0
  b = 1
  while n > 0
    t = a + b
    a = b
    b = t
    n -= 1
  end
  a
end

puts fib(20)
puts fib(40)
begin
  puts fib(200)
rescue RangeError => e
  puts "RangeError: #{e.message}"
end
