# A keyword hash in raise other than a lone `cause:` is one more
# positional argument, and Exception#set_backtrace takes only nil, a
# String or an Array of Strings: anything else is a TypeError.
def t
  yield
  puts "no error"
rescue Exception => e
  puts "#{e.class}: #{e.message}"
end

t { raise(RuntimeError, "w", zz: 2) }
t { raise("m", zz: 2) }
k = :zz
t { raise(RuntimeError, "w", {k => 2}) }
t { raise(RuntimeError, "w", cause: nil, zz: 2) }
t { raise(RuntimeError, zz: 2) }
t { raise(RuntimeError, "w", 5) }
t { fail(ArgumentError, "w", {a: 1}) }
t { raise(RuntimeError, "w", ["a", 1]) }
t { raise(RuntimeError, "w", cause: nil) }
def f(**kw) = raise(RuntimeError, "w", **kw)
t { f }
t { f(cause: nil) }

e = RuntimeError.new("x")
t { e.set_backtrace(5) }
t { e.set_backtrace({a: 1}) }
t { e.set_backtrace([1, 2]) }
e.set_backtrace("one line")
p e.backtrace
e.set_backtrace(["a:1", "b:2"])
p e.backtrace
begin
  raise(RuntimeError, "kept", ["f:9"])
rescue => r
  p [r.message, r.backtrace]
end
