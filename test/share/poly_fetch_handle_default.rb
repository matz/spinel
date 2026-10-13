# fetch with a default on a box (a Hash or an Array) whose default is a
# String the rule shares, a parameter holding its handle: a missing key
# answers that String itself. The dispatch boxed the default as a plain
# String's bytes, and the C did not build.
def pick(sub, key, fallback) = sub.fetch(key, fallback)

s = +"a"
s << "b"
t = pick({ "k" => s }, "x", s)
t << "!"
p s, pick([s], 5, s), pick({ "k" => s }, "k", +"z").equal?(s)
