# to_s, puts, print and interpolation of an Enumerator, a Random and ARGF
# held in a union-typed value render them as their typed to_s does. They
# answered "", so puts printed an empty line. The address in the default
# rendering differs from run to run, so each boxed rendering is compared
# with the typed to_s of the same object.
require "stringio"

def rendered(v)
  io = StringIO.new
  io.puts v
  io.print v, "\n"
  io.puts "#{v}"
  io.puts v.to_s
  io.string.lines.map(&:chomp)
end

def show(name, lines, typed)
  p [name, lines.size, lines.all? { |l| l == typed }]
end

e = [1, 2].each
show("enum|str", rendered(ARGV.empty? ? e : "s"), e.to_s)
show("enum|int", rendered(ARGV.empty? ? e : 1), e.to_s)
show("enum|nil", rendered(ARGV.empty? ? e : nil), e.to_s)
s = (1..3).each_slice(2)
show("each_slice|str", rendered(ARGV.empty? ? s : "s"), s.to_s)
show("enum in array", [e, 0].map { |v| v.to_s }.first(1), e.to_s)

r = Random.new(1)
show("random|str", rendered(ARGV.empty? ? r : "s"), r.to_s)

a = ARGV.empty? ? ARGF : "s"
p rendered(a)
puts a
print a, "\n"

# the other kind of the union still renders as it did
p rendered(ARGV.empty? ? "s" : e)
p rendered(ARGV.empty? ? 7 : r)

# a temporary held by nothing else
def mk(i) = i.even? ? [i, i + 1].each : i
n = 0
300.times { |i| n += 1 if mk(i).to_s.start_with?("#<Enumerator") }
p n
