# Process.clock_gettime / clock_getres with a unit that is not a Symbol
# literal: a local, a value read from an Array, a keyword hash, a String.
# A Symbol unit gives the same class as the literal; anything else is
# ArgumentError "unexpected unit".
def t(label)
  r = yield
  puts "#{label}: #{r.class}"
rescue ArgumentError, TypeError => e
  puts "#{label}: #{e.class}: #{e.message}"
end

id = Process::CLOCK_MONOTONIC
[:nanosecond, :microsecond, :millisecond, :second,
 :float_microsecond, :float_millisecond, :float_second].each do |u|
  t("gettime #{u}") { Process.clock_gettime(id, u) }
  t("getres #{u}") { Process.clock_getres(id, u) }
end
u = :millisecond
t("local") { Process.clock_gettime(id, u) }
t("nil") { Process.clock_gettime(id, nil) }
t("kwargs") { Process.clock_gettime(id, x: 1) }
t("hash") { Process.clock_gettime(id, {x: 1}) }
t("string") { Process.clock_gettime(id, "millisecond") }
t("integer") { Process.clock_gettime(id, 5) }
v = :bogus
t("bad symbol") { Process.clock_gettime(id, v) }
t("getres kwargs") { Process.clock_getres(id, x: 1) }

# the values agree with the literal unit
a = Process.clock_gettime(id, :millisecond)
b = Process.clock_gettime(id, u)
c = Process.clock_gettime(id, :millisecond)
puts a <= b && b <= c
s = :second
puts (Process.clock_gettime(id, s) - Process.clock_gettime(id, :second)).abs <= 1
f = :float_second
puts (Process.clock_gettime(id, f) - Process.clock_gettime(id)).abs < 1.0
