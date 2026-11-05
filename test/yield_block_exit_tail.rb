# A block that ends in exit or abort has no value: the call it is handed
# to raises SystemExit there, whatever reads the call's value and whatever
# kind the method's own rescue answers.

def g
  yield
rescue => e
  e.message
end

def gi
  yield
rescue
  7
end

def status
  yield
  :no_exit
rescue SystemExit => e
  [e.status, e.success?]
end

# a String: a local, an argument, an interpolation, p
def id(s) = s
p status { x = g { exit 2 }; p x }
p status { p id(g { exit }) }
p status { puts "v=#{g { exit false }}" }
p status { p g { exit true } }

# ...and where a String is kept
out = ["a"]
h = { "a" => "1" }
p status { out << g { exit 3 } }
p status { out.push(g { exit 4 }) }
p status { lit = [g { exit 5 }, "z"]; p lit }
p status { h["k"] = g { exit 6 } }
p out, h

# an Integer
nums = [1]
p status { x = gi { exit 7 }; p x }
p status { p(gi { exit 8 } + 1) }
p status { nums << gi { exit 9 } }
p nums

# statements before the exit run; exit! is not tried here, it ends the process
n = 0
p status { g { n += 1; exit n + 10 } }
p n

# a yield with an argument, a block parameter's call, an ensure
def gv(v)
  yield v
rescue => e
  e.message
end
def gc(v, &blk)
  blk.call(v)
rescue => e
  e.message
end
def ge
  yield
rescue => e
  e.message
ensure
  puts "ensure"
end
p status { x = gv(12) { |s| exit s }; p x }
p status { x = gc(13) { |s| exit s }; p x }
p status { x = ge { exit 14 }; p x }

# the method's last expression, an instance variable
def tail
  g { exit 15 }
end
class Keep
  def g
    yield
  rescue => e
    e.message
  end
  def run
    @v = g { exit 16 }
  end
end
p status { p tail }
p status { Keep.new.run }

# beside a block that answers a value
p status { p g { "v" }; p g { exit 17 } }

# abort writes its message and exits 1
p status { x = g { abort "first" }; p x }
p status { out << g { abort "second" } }
p status { x = gi { abort }; p x }
p out

# with nothing to rescue it, the exit ends the program
at_exit { puts "at_exit" }
x = g { exit }
puts "not reached #{x}"
