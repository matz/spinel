# A threaded render, sized so the collector is the thing being measured.
#
# Nothing else in benchmark/ exercises the collector under concurrency: the
# suite is single-threaded, so the phases that only exist when workers are
# parked -- the parallel slot sweep, the per-worker string budget, the root
# walk over other workers' fibers -- are never entered by anything the gate
# runs. This is that workload, and its shape is copied from the application
# that raised #4384 rather than invented: a web request that RETURNS its
# markup up a nest of views, holding every fragment it has produced so far
# live while it produces the next one, at concurrency.
#
# Four properties are load-bearing. A change that makes this program faster
# by removing one of them has removed the reason it is here:
#
#   1. NESTING, WITH THE CALLER RETAINING.  page -> room -> message -> body
#      -> span, each level returning a String its caller keeps. The live set
#      at any instant is therefore one partial buffer PER LEVEL per request
#      in flight, not one buffer -- which is what puts a request's whole
#      render on the mark's list rather than just its last few kilobytes.
#   2. THE YIELD.  render_room calls Thread.pass with the page's fragment
#      list, the room's buffer and the message's spans all live. Without it
#      no two requests overlap, and the live set stops growing with the
#      number of threads, which is the axis this program exists to walk.
#   3. FRESHLY ALLOCATED TEXT.  Every string interpolates the request id, so
#      no two requests share one. Rendering from a fixture would pin the live
#      set flat and the concurrency axis would go away with it.
#   4. STRINGS DOMINATE THE LIVE BYTES. Small objects keep large Strings.
#      The current collector charges the String handles and payloads to the
#      same budget as the rest of the graph.
#
# What it shows. The claim in #4384 is that the mark does not parallelize:
# its per-collection cost RISES as workers are added while the slot sweep
# beside it falls. Reading that off a ladder needs the pacing held still,
# because the adaptive budget scales per worker and a wider budget collects
# less often with more live at each collection -- which moves the mark for a
# reason that is not the mark's. So the aggregate budget is pinned across the
# ladder. The allocator scales the configured base by the worker limit, so
# the base below divides the aggregate budget by that limit:
#
#   SPINEL_GC_OBJ_BUDGET=fixed \
#   SPINEL_GC_THRESHOLD_OBJ_KB=$((65536/W)) \
#   SPINEL_WORKERS=$W SPINEL_GC_PHASES=1
#
# Measurements below used the older split-heap collector on Apple M-series
# hardware with six performance cores. In those rows, `root walk` is the
# roots + fibers + globals fields of the [gcph] mark split and `scan` is
# the trace.
#
#   W    colls   objs/coll   mark/coll   root walk   scan      ns/obj   slot sweep/coll
#   1     37      182,158      2.81 ms    0.000 ms   2.81 ms    15.4       5.89 ms
#   2     38      183,220      3.34 ms    0.000 ms   3.34 ms    18.2       4.24 ms
#   4     37      182,923      3.51 ms    0.000 ms   3.49 ms    19.2       3.14 ms
#   8     38      182,717      3.66 ms    0.000 ms   3.63 ms    20.0       2.95 ms
#   12    36      177,930      3.58 ms    0.000 ms   3.56 ms    20.1       2.39 ms
#
# The first three columns are the control: the pacing and the live graph are
# flat, so neither phase is handed more work. Over that range the slot sweep
# falls to 0.41x -- it is the phase that gets the parked workers, and it
# behaves -- while the mark rises 1.27x. Same signs as the application ladder
# in #4384 (mark 2.04x, slot sweep 0.51x), on a smaller machine.
#
# The root walk is 0.000 ms at every worker count, so on this shape the rise
# is not more stacks to scan: it is entirely in `scan`, at 15.4 -> 20.1 ns per
# object marked over a graph that did not grow. Slicing the fiber list to the
# parked workers would reach the column that is already zero.
#
# Run WITHOUT the pins and the mark still reads as rising, but part of that is
# the budget letting more accumulate between collections rather than the mark
# costing more. Both ladders are worth having; only the pinned one is
# evidence.
#
# THE ANSWER IS DETERMINISTIC. Each thread's work is fixed by its index, so
# the digests below do not depend on how the threads interleave, on how many
# workers run them, or on when a collection lands in the middle of one. A
# collector that drops a live fragment, hands one thread another's buffer, or
# sweeps a string still reachable through a retained Span changes a digest
# rather than only a timing -- which is the point of computing them at all.

CONCURRENCY = 32          # green threads, i.e. requests in flight; NOT workers
REQUESTS_PER_THREAD = 8
ROOMS = 12
MESSAGES_PER_ROOM = 40
WORDS_PER_MESSAGE = 24
WORD_REPEAT = 6           # widens each span's text without adding an object

AUTHORS = ["ada", "brendan", "grace", "matz", "yukihiro", "linus", "rich", "barbara"]
KINDS = ["text", "quote", "code", "link"]
WORDS = ["gc", "mark", "sweep", "budget", "worker", "thread", "string", "slot",
         "heap", "trigger", "live", "root", "safepoint", "barrier"]

# The fixture is the whole of the long-lived object set: 40 messages, read by
# every request and never written. Everything a request retains, it allocated.
class Message
  attr_reader :id, :author, :kind, :words
  def initialize(id, author, kind, words)
    @id = id
    @author = author
    @kind = kind
    @words = words
  end
end

# A rendered span, kept by the message that produced it. These are the objects
# the mark walks; the bytes they point at are what the string heap holds.
class Span
  attr_reader :index, :html
  def initialize(index, html)
    @index = index
    @html = html
  end
end

# One rendered message, kept by the page until the page is assembled.
class Fragment
  attr_reader :room, :id, :html, :spans
  def initialize(room, id, html, spans)
    @room = room
    @id = id
    @html = html
    @spans = spans
  end
end

MESSAGES = (0...MESSAGES_PER_ROOM).map do |i|
  words = (0...WORDS_PER_MESSAGE).map { |j| WORDS[(i * 7 + j * 5) % WORDS.size] }
  Message.new(i, AUTHORS[i % AUTHORS.size], KINDS[i % KINDS.size], words)
end

def render_span(req, msg, word, k)
  "<span class=\"w#{k} #{msg.kind}\" data-req=\"#{req}\" data-msg=\"#{msg.id}\">" \
    "#{word * WORD_REPEAT}</span>"
end

def render_body(req, msg, spans)
  out = +"<p class=\"body #{msg.kind}\" data-req=\"#{req}\">"
  msg.words.each_with_index do |w, k|
    html = render_span(req, msg, w, k)
    spans << Span.new(k, html)   # the span outlives the concatenation
    out << html
  end
  out << "</p>"
  out
end

def render_message(req, room, msg, spans)
  out = +"<article id=\"m-#{req}-#{room}-#{msg.id}\" class=\"msg #{msg.kind}\">"
  out << "<header><a class=\"author\" href=\"/u/#{msg.author}\">#{msg.author}</a>"
  out << "<time datetime=\"2026-09-09T#{room}:#{msg.id}\">#{req}</time></header>"
  out << render_body(req, msg, spans)
  out << "<footer class=\"actions\"><button data-id=\"#{req}-#{msg.id}\">reply</button></footer>"
  out << "</article>"
  out
end

def render_page(req)
  parts = ["<!doctype html><html><head><title>request #{req}</title></head><body>"]
  ROOMS.times do |room|
    parts << "<section class=\"room\" id=\"r-#{req}-#{room}\"><h2>room #{room} of #{req}</h2>"
    MESSAGES.each_with_index do |msg, i|
      spans = []
      parts << Fragment.new(room, msg.id, render_message(req, room, msg, spans), spans)
      # Yield with the page's fragments, this room's spans and the caller's
      # buffers all live. This is the moment another thread's allocation is
      # most likely to trigger a collection, and the reason the program has
      # anything to say about the mark.
      Thread.pass if (i & 7) == 7
    end
    parts << "</section>"
  end
  parts << "</body></html>"

  out = +""
  parts.each { |p| out << (p.is_a?(Fragment) ? p.html : p) }
  out
end

# Position-sensitive, so a page that is the right length with the wrong bytes
# in it still fails. Chunked rather than per-byte because a per-byte fold in
# Ruby would cost more than the render it is checking.
def digest(s)
  h = 2166136261
  i = 0
  n = s.bytesize
  while i < n
    h = (h * 31 + s.byteslice(i, 256).sum) % 1000000007
    i += 256
  end
  h
end

threads = (0...CONCURRENCY).map do |t|
  Thread.new(t) do |tid|
    d = 0
    bytes = 0
    REQUESTS_PER_THREAD.times do |r|
      page = render_page(tid * 1000 + r)
      bytes += page.bytesize
      d = (d * 131 + digest(page)) % 1000000007
    end
    [d, bytes]
  end
end

total = 0
threads.each_with_index do |th, i|
  d, bytes = th.value
  total += bytes
  puts "thread #{i}: bytes=#{bytes} digest=#{d}"
end
puts "total bytes=#{total}"
