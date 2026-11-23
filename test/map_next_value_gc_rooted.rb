# A map holds each block value in a temp until it is stored. A value that
# leaves through an `ensure` (a `next`, or the tail inside the begin) waits
# there while the ensure runs, and an ensure that allocates can collect: the
# temp is a GC root, so the value (a fresh Array, Hash, String, Struct or
# method result, boxed or typed) survives to the store. Checked with
# SPINEL_GC_STRESS=1 and 2.

$log = []
def mk(x) = [x.to_s, x]
def noise(x)
  $log << ("e#{x}" * 3)
  [1, 2, 3].map { |z| z.to_s * 5 }
end

def ewi(a) = a.each_with_index.map { |x, i| begin; next [x, i] if i.even?; x; ensure; noise(i); end }
def ewih(a) = a.each_with_index.map { |x, i| begin; next({k: x}) if i.even?; x; ensure; noise(i); end }
def plain(a) = a.map { |x| begin; next mk(x) if x.odd?; x; ensure; noise(x); end }
def slices(a) = a.each_slice(2).map { |p, q| begin; next [p, q, 0] if p.odd?; p; ensure; noise(p); end }
def hmap(h) = h.map { |k, v| begin; next [k, v] if v.odd?; v; ensure; noise(v); end }

# typed results: a String, an Integer Array, a Struct
Pt = Struct.new(:v)
def ewis(a) = a.each_with_index.map { |x, i| begin; next "n#{x}" * 2 if i.even?; "t#{x}" * 2; ensure; noise(i); end }
def plains(a) = a.map { |x| begin; next "n#{x}" * 2 if x.even?; "t#{x}" * 2; ensure; noise(x); end }
def plainr(a) = a.map { |x| begin; next [x, 1] if x.odd?; [0]; ensure; noise(x); end }
def ewio(a) = a.each_with_index.map { |x, i| begin; next Pt.new(x * 11) if i.odd?; Pt.new(0); ensure; noise(i); end }
def slicer(a) = a.each_slice(2).map { |p, q| begin; next [p, q] if p.odd?; [0]; ensure; noise(p); end }
def hmapr(h) = h.map { |k, v| begin; next [v, v] if v > 2; [0]; ensure; noise(v); end }
def hmaps(h) = h.map { |k, v| begin; next "v#{v}" * 2 if v.odd?; k; ensure; noise(v); end }

a = (1..24).to_a
h = {}
24.times { |i| h["k#{i}"] = i }
r = nil
60.times { r = [ewi(a), ewih(a), plain(a), slices(a), hmap(h), ewis(a), plains(a), plainr(a), ewio(a).map(&:v), slicer(a), hmapr(h), hmaps(h)] }
r.each { |x| p x.first(4), x.size }
p $log.size
