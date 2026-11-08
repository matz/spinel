# `Kernel.printf` while $stdout is reassigned writes Kernel#format's text,
# also where the program defines a format of its own. A bare printf there
# is refused, since its text could come from that format; the one written
# on Kernel cannot, and was refused with it.
require "stringio"

class Log
  def format(*a) = "OWN"
  def go = Kernel.printf("%d-", 8)
end

def format(*a) = "top"

io = StringIO.new
$stdout = io
Log.new.go
Kernel.printf("%03d|", 5)
$stdout = STDOUT
p io.string
