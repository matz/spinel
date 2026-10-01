# A call to a method no class defines raises NoMethodError at run time, and
# its receiver is evaluated first, as CRuby evaluates it before the lookup.
# A receiver the message cannot stage (anything but a bare name or a
# literal) was never emitted: Foo.new did not run, and a receiver that is
# itself such a call never raised its own error -- the outer call reported
# its own name on "unknown".
class Foo
  def initialize
    puts "Foo.new ran"
  end

  def n
    puts "n ran"
    3
  end

  def s
    puts "s ran"
    "str"
  end

  def x
    puts "x ran"
    1.5
  end
end

def try
  yield
rescue NoMethodError => e
  puts e.message
end

try { Foo.new.bar }
try { p Foo.new.bar.size }
f = Foo.new
try { p f.bar.baz.size }

$log = []
def make
  $log << :made
  Foo.new
end
try { make.missing }
p $log

# A receiver that answers an Integer, a String or a Float runs once: the
# scalar arms rendered it before giving the call up, and the prelude they
# left behind ran Foo.new a second time.
try { Foo.new.n.missing }
try { Foo.new.s.missing }
try { Foo.new.x.missing }
