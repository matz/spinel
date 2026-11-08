# `Kernel.puts` writes to a reassigned $stdout, as a bare puts does, and
# so does one inside a class that defines its own puts. The Kernel
# receiver is dropped and the bare call is rewritten to `$stdout.puts`,
# but the rewritten call kept the mark that made it Kernel's own puts
# (NoMethodError on the StringIO), and beside a puts of the program's it
# was not rewritten at all.
require "stringio"

class Log
  def puts(m) = Kernel.puts("[#{m}]")
  def print(m) = Kernel.print("<#{m}>")
  def p(x) = Kernel.p(x)
  def printf(f, x) = Kernel.printf(f, x)
end

io = StringIO.new
$stdout = io
l = Log.new
l.puts("hi")
l.print("pr")
r = l.p(3)
l.printf("%02d\n", 4)
Kernel.puts "plain"
Kernel.print "k", "\n"
$stdout = STDOUT
p io.string
p r
