# A server's request garbage and the full-collection cadence. Sixteen
# requests are in flight against a live set of a few MB, served a step at a
# time, so each request's objects survive a few minors, are promoted, and die
# when its response goes out. Measured against the live set that is far above
# the promoted-garbage cut (a third of it per minor), and the cadence used to
# fall to a full on four collections in five. In bytes it is a few MB, under
# the slack the old generation is allowed anyway, so the minors keep running.
# The second phase promotes tens of MB per minor the same way: there the
# cadence still shortens and the old generation stays bounded.
class Req
  attr_reader :headers, :parts
  def initialize(id, pad)
    @headers = {}
    8.times { |h| @headers["x-header-" + h.to_s] = "value-" + id.to_s + "-" + h.to_s }
    @parts = []
    @pad = pad > 0 ? "x" * pad : nil
  end
end

routes = {}
3000.times { |i| routes["/articles/" + i.to_s] = ["Article " + i.to_s, "body " * 20, i] }

def serve(routes, inflight, steps, n, pad)
  slots = Array.new(inflight)
  served = 0
  i = 0
  while i < n
    k = i % inflight
    r = slots[k]
    if r.nil? || r.parts.length >= steps
      served += 1 if r && r.parts.join.length > 0
      r = Req.new(i, pad)
      slots[k] = r
    end
    row = routes["/articles/" + ((i * 7) % 3000).to_s]
    r.parts << "<li><a href=\"/articles/" + row[2].to_s + "\">" + row[0] + "</a></li>\n"
    i += 1
  end
  served
end

# GC.stat's "cycle", "full_runs" and "old_bytes" are Spinel's; under CRuby,
# which has neither this cadence nor these keys, the checks read as passing.
SPINEL_GC = GC.stat.key?("full_runs")
def gc_reading
  s = GC.stat
  SPINEL_GC ? [s["cycle"], s["full_runs"], s["old_bytes"]] : [0, 0, 0]
end
def verdict(ok)
  (!SPINEL_GC || ok) ? "yes" : "no"
end

g0 = gc_reading
served = serve(routes, 16, 300, 1_500_000, 0)
g1 = gc_reading
cycles = g1[0] - g0[0]
fulls = g1[1] - g0[1]
puts "served: " + served.to_s
puts "collections ran: " + verdict(cycles > 100)
# Before: 81% full. A cadence of 8 is 12.5%.
puts "most collections are minors: " + verdict(fulls * 2 < cycles)

served = serve(routes, 256, 40, 300_000, 4096)
g2 = gc_reading
cycles = g2[0] - g1[0]
fulls = g2[1] - g1[1]
puts "served: " + served.to_s
puts "large promoted garbage shortens the cadence: " + verdict(fulls * 4 > cycles)
puts "old generation bounded: " + verdict(g2[2] < 64_000_000)
