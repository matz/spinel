# A frozen literal is one String object per content: every evaluation of
# a literal site, and every site with the same text, answers the same
# frozen object, whatever holds it and whichever face of it a builtin
# reads. Here: identity across calls, loops, blocks, a Fiber and a
# Thread; the empty, one-character, multibyte, NUL-carrying and
# invalid-byte literals; dup, +@, -@, clone and freeze; the FrozenError a
# mutation raises; object_id; interpolated against plain literals; and a
# literal used only as builtin bytes; and a literal's hash, which its
# header holds from the start, against the same bytes built at run time.
def site = "abc"
ids = []
3.times { ids << site.object_id }
i = 0
while i < 2
  ids << "abc".object_id
  i += 1
end
p ids.uniq.size, [1, 2].map { "abc" }.uniq(&:object_id).size

def a = "same"
def b = "same"
p a.equal?(b), ["same", "same"].uniq(&:object_id).size

def e = ""
def one = "x"
def mb = "héllo"
def nul = "a\0b"
def hi = "\xff\xfe"
[e, one, mb, nul, hi].each do |s|
  p [s.equal?(s), s.frozen?, s.bytesize, s.length, s.valid_encoding?]
end
held = [e, one, mb, nul]
p held[0].equal?(e), held[1].equal?(one), held[2].equal?(mb), held[3].equal?(nul)
p({ e => 1, one => 2, mb => 3 }["héllo"], mb.upcase, nul.inspect)

def l = "q"
d = l.dup
u = +l
c = l.clone
p d.equal?(l), d.frozen?, u.equal?(l), u.frozen?, (-l).equal?(l), c.equal?(l), l.freeze.equal?(l)
u << "!"
p u, l, (-(+"q")).equal?(l), l.to_s.equal?(l), String(l).equal?(l)

def frz = "frz"
[:upcase!, :clear, :succ!].each do |m|
  begin
    frz.send(m)
  rescue FrozenError => ex
    p [m, ex.message]
  end
end
begin
  [frz][0] << "x"
rescue FrozenError => ex
  p ex.message, ex.receiver.equal?(frz)
end

def oid = "oid"
o1 = oid.object_id
GC.start
p o1 == oid.object_id, [1, oid][1].object_id == o1, oid.dup.object_id == o1

def ft = "ft"
f = Fiber.new { Fiber.yield ft; ft }
x = f.resume
y = f.resume
z = Thread.new { ft }.value
p x.equal?(ft), y.equal?(ft), z.equal?(ft)

def interp(n) = "v#{n}"
p interp(1).equal?(interp(1)), interp(1).frozen?, ("a" "b").equal?("ab")

s = "a,b,c"
p s.split(","), s.index("c"), s.sub("a", "z")
putc "|"
puts

a = "a"
h = { "ab" => 1, "" => 2, "café" => 3, "t\tq\"b\\" => 4, "n\0b" => 5, "x" "y" => 6, "\r\n" => 7, "\xff\xfe" => 8 }
p h[a + "b"], h[a[0, 0]], h["caf" + "é"], h["t\t" + "q\"b\\"], h["n" + "\0b"], h[a.tr("a", "x") + "y"], h["\r" + "\n"], h["\xff" + "\xfe"]
p "ab".hash == (a + "b").hash, "n\0b".hash == ("n" + "\0b").hash, "\xff\xfe".hash == ("\xff" + "\xfe").hash
p "café".length, "\xff\xfe".length, "\xff\xfe".valid_encoding?, "\xff\xfe".length
