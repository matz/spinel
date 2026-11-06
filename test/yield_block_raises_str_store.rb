# A block that ends in a raise has no value, and the method that yields to
# it answers what its own rescue answers. Stored where a String is kept (an
# Array of Strings, a Hash of them), that answer is the rescue's String.

def g
  yield
rescue => e
  e.message
end

out = []
out << g { raise "b" }
p out

out.push(g { fail "c" })
out.unshift(g { raise ArgumentError, "d" })
p out

lit = [g { raise "e" }, "f"]
p lit

h = { "a" => "1" }
h["k"] = g { raise "g" }
p h

# statements before the raise run
n = 0
out << g { n += 1; raise "h#{n}" }
p out, n

# a raise with nothing to raise again is a RuntimeError
def cls
  yield
rescue => e
  e.class.to_s
end
out << cls { raise }
p out.last

# the block's parameter
def gp(v)
  yield v
rescue => e
  e.message + v
end
out << gp("q") { |s| raise "i" + s }
p out.last

# a block called through the method's block parameter
def gc(s, &blk)
  blk.call(s)
rescue => e
  e.message
end
out << gc("r") { |z| raise "j" + z }
p out.last

# an ensure beside the rescue
def ge
  yield
rescue => e
  e.message
ensure
  puts "ensure"
end
out << ge { raise "k" }
p out.last

# a begin in the method's body
def gb
  r = begin
    yield
  rescue => e
    e.message
  end
  r + "!"
end
out << gb { raise "l" }
p out.last

# a method of an object
class W
  def g
    yield
  rescue => e
    e.message
  end
end
out << W.new.g { raise "m" }
p out.last

# beside a block that answers a String a variable holds: the Array keeps
# that String, and the raising block's call still answers the rescue's
s = +"v"
both = []
both << g { s }
both << g { raise "n" }
s << "!"
p both

# a raise in each arm
both << g { if both.size > 1 then raise "o" else raise "p" end }
p both.last
