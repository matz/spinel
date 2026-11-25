# In a program that freezes an IO: p, puts and print write an inspect or
# to_s result with its NUL byte, `x = puts()` writes one newline, and p and
# pp in value position raise FrozenError on a frozen STDOUT
require "tmpdir"

class Nul
  def inspect = "a\0b"
  def to_s = "c\0d"
end

class Seen
  def inspect
    $seen += 1
    "seen"
  end
end

def show(v) = p(v)

out = STDOUT.dup
out.sync = true
path = File.join(Dir.tmpdir, "io_frozen_value_forms_#{Process.pid}.txt")
STDOUT.reopen(path, "w")

def written(out, path)
  STDOUT.flush
  out.puts File.binread(path).bytes.inspect
  File.truncate(path, 0)
  STDOUT.rewind
end

n = Nul.new
p n
written(out, path)
puts n
written(out, path)
print n
written(out, path)
p n, n
written(out, path)
ns = [n]
p(*ns)
written(out, path)
puts(*ns)
written(out, path)
print(*ns)
written(out, path)
x = p n
written(out, path)
x = p n, n
written(out, path)

x = puts()
out.puts x.inspect
written(out, path)

x = p(1)
out.puts x.inspect
x = p(1, 2)
out.puts x.inspect
x = [p(3)]
out.puts x.inspect
x = show(4)
out.puts x.inspect
x = pp(5)
out.puts x.inspect
written(out, path)

STDOUT.freeze
$seen = 0
s = Seen.new

def try(out)
  r = yield
  out.puts "returned #{r.inspect}"
rescue FrozenError => e
  out.puts "#{e.class} #{$seen}"
end

try(out) { x = puts(); x }
try(out) { x = p(s); x }
try(out) { x = p(s, s); x }
try(out) { [p(s)] }
try(out) { show(s) }
try(out) { x = pp(s); x }
try(out) { x = p(); x }
File.delete(path)
