# A block whose value is a call no class answers lowers that call to its
# NoMethodError raise. Spliced into a yield whose value another call site's
# block typed (an Integer, a String, ...), the raise's boxed value went into
# that typed slot and the C did not compile. It raises, as in CRuby.
class Foo
end

def try
  p yield
rescue NoMethodError => e
  puts e.message
end

try { 1 }
try { Foo.new.bar }

def try_s
  p yield
rescue NoMethodError => e
  puts e.message
end

try_s { "s" }
f = Foo.new
try_s { x = 2; f.upcase }

# The tail in parentheses, and an undefined bare name (a NameError), in an
# Integer slot; and a Hash slot.
def try_n
  p yield
rescue NameError => e
  puts e.class
end

try_n { 1 }
try_n { (Foo.new.bar) }
try_n { nope }

def try_h
  p yield
rescue NoMethodError => e
  puts e.message
end

try_h { { a: 1 } }
try_h { Foo.new.bar }
