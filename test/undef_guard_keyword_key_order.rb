# A call to a builtin that a conditional undef may remove holds its operands
# in CRuby's order, each pair's key ahead of its value. With the undef not
# run, a key read ahead of a value that writes it keeps the value it had.
# With the undef run, the key and the value still run, and then the call
# raises NoMethodError, whatever shape the key has.
$log = []
def side(x) = ($log << x; x)

if ARGV.size == 9
  class << self; undef_method :p; end
end
k = :before
p(k => (k = :after))
@iv = :before
p(@iv => (@iv = :after))
p(side(:k) => side(:v))
p(("k".dup) => 1)
p k
p $log

if ARGV.size == 0
  class << self; undef_method :puts; end
end
$log = []
j = :before
r = (puts(j => (j = :after)) rescue $!.class)
$stdout.puts [r, j].inspect
r = (puts(("k".dup) => 1) rescue $!.class)
$stdout.puts r.inspect
r = (puts(side(:k) => side(:v)) rescue $!.class)
$stdout.puts r.inspect
$stdout.puts $log.inspect
r = (puts(1) rescue $!.class)
$stdout.puts r.inspect
