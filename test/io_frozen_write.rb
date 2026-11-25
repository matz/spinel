# A write to a frozen IO raises FrozenError; a frozen STDOUT raises from
# Kernel#p, #puts, #print and #printf too
require "tmpdir"

out = STDOUT.dup
out.sync = true

def try(out)
  yield
  out.puts "wrote"
rescue FrozenError => e
  out.puts e.message.sub(/File:.*>\z/, "File:...>")
end

path = File.join(Dir.tmpdir, "io_frozen_write_#{Process.pid}.txt")
f = File.open(path, "w").freeze
out.puts f.frozen?
try(out) { f.write "a" }
try(out) { f.write "" }
try(out) { f.puts "a" }
try(out) { f.print "a" }
try(out) { f << "a" }
try(out) { f.syswrite "a" }
try(out) { f.printf("%d", 1) }
try(out) { f.write("", "") }
try(out) { f.write }
one = [""]
two = ["", ""]
try(out) { f.write(*one) }
try(out) { f.write(*two) }
out.puts f.fileno > 2
f.close
g = File.open(path, "w")
g.close
g.freeze
try(out) { g.write "a" }
try(out) { g.puts "a" }
try(out) { g.print "a" }
try(out) { g << "a" }
try(out) { g.syswrite "a" }
try(out) { g.printf("%d", 1) }
try(out) { g.write("", "") }
try(out) { g.write }
try(out) { g.write(*two) }
File.delete(path)

x = 1
r = p x, (x = 2)
out.puts r.inspect

out.puts STDOUT.frozen?
try(out) { p STDOUT.freeze.fileno }
out.puts STDOUT.frozen?
try(out) { puts "a" }
try(out) { puts }
try(out) { print "a" }
try(out) { printf("%d\n", 1) }
try(out) { $stdout.write "a" }
try(out) { STDOUT.puts "a" }
try(out) { STDOUT << "a" }
try(out) { print "" }
words = ["a", "b"]
none = []
try(out) { puts(*words) }
try(out) { print(*words) }
try(out) { puts(*none) }
empty = [""]
try(out) { print(*none) }
try(out) { print(*empty) }
try(out) { p(*none) }
try(out) { p }
try(out) { pp(*none) }
try(out) { r = puts(*words); out.puts r.inspect }
try(out) { r = print(*words); out.puts r.inspect }
try(out) { r = print(*empty); out.puts r.inspect }
try(out) { r = p(*words); out.puts r.inspect }
try(out) { r = p(*none); out.puts r.inspect }
nested = [[]]
try(out) { puts(*nested) }
later = ["a"]
try(out) { puts(*(later << "b")) }
out.puts later.inspect
more = [1]
try(out) { puts(*none, *more.clear) }
out.puts more.inspect
try(out) { printf("") }
some = [1]
try(out) { p(*(some << 2)) }
out.puts some.inspect
zero = [0]
try(out) { printf("%c", *zero) }
$byte = nil
try(out) { putc($byte = 65) }
out.puts $byte.inspect
$char = nil
try(out) { putc(($char = "b")) }
out.puts $char.inspect
try(out) { putc("") }
try(out) { putc("\0") }
$last = nil
try(out) { p(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, ($last = 17)) }
out.puts $last.inspect
