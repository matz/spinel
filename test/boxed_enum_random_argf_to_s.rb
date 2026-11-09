# to_s, puts, print and interpolation of an Enumerator, a Random and ARGF
# held in a union-typed value render them as their typed to_s does. They
# answered "", so puts printed an empty line. The address in the default
# rendering differs from run to run, so the Enumerator and the Random are
# checked by their prefix.
require "stringio"

def rendered(v)
  io = StringIO.new
  io.puts v
  io.print v, "\n"
  io.puts "#{v}"
  io.puts v.to_s
  io.string.lines.map(&:chomp)
end

def show(name, lines, prefix)
  p [name, lines.size, lines.all? { |l| l.start_with?(prefix) }]
end

e = [1, 2].each
show("enum|str", rendered(ARGV.empty? ? e : "s"), "#<Enumerator")
show("enum|int", rendered(ARGV.empty? ? e : 1), "#<Enumerator")
show("enum|nil", rendered(ARGV.empty? ? e : nil), "#<Enumerator")
show("each_slice|str", rendered(ARGV.empty? ? (1..3).each_slice(2) : "s"), "#<Enumerator")
show("enum in array", [e, 0].map { |v| v.to_s }.first(1), "#<Enumerator")

r = Random.new(1)
show("random|str", rendered(ARGV.empty? ? r : "s"), "#<Random:")

a = ARGV.empty? ? ARGF : "s"
p rendered(a)
puts a

# the other kind of the union still renders as it did
p rendered(ARGV.empty? ? "s" : e)
p rendered(ARGV.empty? ? 7 : r)

# a temporary held by nothing else
def mk(i) = i.even? ? [i, i + 1].each : i
n = 0
300.times { |i| n += 1 if mk(i).to_s.start_with?("#<Enumerator") }
p n
