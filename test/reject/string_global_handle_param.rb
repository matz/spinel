# A global handed to a parameter that takes a String's handle and appends
# to it: a global has no handle to give, so the parameter would bind a
# fresh String of its bytes and the append would not reach the global's.
# Refused rather than compiled with the append lost. The global is run
# first here, as a later argument assigns it, which is the binder path a
# variable holding no handle takes.
module Helper
  def self.open_into(io) = (io << "<div>"; nil)
end
Helper.open_into([]) if ARGV.size > 5   # a second caller makes io POLY
def h(k0, io, k) = (Helper.open_into(io); io << "h#{k0}#{k}"; nil)
buf = +""
$d = String.new
keep = $d
h((buf << "ab"; buf.upcase!; buf.size), $d, ($d = String.new; 1))
p [$d, keep, buf]
