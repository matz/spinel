# A block that leaves by throw (or exit, or abort) has no value, and a
# method that only yields to it answers none. The call still stands where
# a String or an Integer is kept or used: nothing reaches the slot, and
# the throw is caught where its catch is.

def t
  yield
end

def tv(v)
  yield v
end

def tc(v, &blk)
  blk.call(v)
end

def te
  yield
ensure
  puts "ensure"
end

def caught
  catch(:x) { yield; :not_thrown }
end

# where a String is kept
out = ["a"]
h = { "a" => "1" }
p caught { out << t { throw :x, "s1" } }
p caught { out.push(t { throw :x, "s2" }) }
p caught { out.unshift(t { throw :x, "s3" }) }
p caught { out.insert(0, t { throw :x, "s4" }) }
p caught { out[0] = t { throw :x, "s5" } }
p caught { lit = [t { throw :x, "s6" }, "z"]; p lit }
p caught { h["k"] = t { throw :x, "s7" } }
p caught { s = t { throw :x, "s8" }; p s }
p out, h

# where an Integer is kept or used
nums = [1]
hi = { "a" => 1 }
p caught { nums << t { throw :x, 1 } }
p caught { hi["k"] = t { throw :x, 2 } }
p caught { lit = [t { throw :x, 3 }, 2]; p lit }
p caught { n = 1 + t { throw :x, 4 }; p n }
p caught { n = t { throw :x, 5 }; p n }
p caught { p (1..t { throw :x, 6 }).to_a }
p nums, hi

# a Float, and an Array of objects
class Item; end
items = [Item.new]
p caught { f = 1.5 + t { throw :x, 7 }; p f }
p caught { items << t { throw :x, 8 } }
p items.size

# a throw with no value, statements before the throw
n = 0
p caught { out << t { throw :x } }
p caught { out << t { n += 1; throw :x, "n#{n}" } }
p n

# a yield with an argument, a block parameter's call, an ensure
p caught { out << tv("q") { |s| throw :x, s + "1" } }
p caught { out << tc("q") { |s| throw :x, s + "2" } }
p caught { hi["k"] = tc(2) { |i| throw :x, i + 1 } }
p caught { lit = [tc("q") { |s| throw :x, s + "3" }, "z"]; p lit }
p caught { out << te { throw :x, "s9" } }

# a throw nothing catches
begin
  out << t { throw :nope, "v" }
rescue UncaughtThrowError => e
  p e.message
end

# exit and abort leave the same way
def status
  yield
  :no_exit
rescue SystemExit => e
  e.status
end
p status { out << t { exit 3 } }
p status { out.push(t { exit }) }
p status { lit = [t { exit 4 }, "z"]; p lit }
p status { hi["k"] = t { exit 5 } }
p status { out << t { abort "aborted" } }
p out, hi
