# A `**` operand to a builtin that takes no keywords passes nothing when its
# hash is empty and one more positional when it is not, so a count CRuby
# refuses then raises ArgumentError
def t(l)
  r = yield
  puts "#{l}: #{r.inspect}"
rescue => e
  puts "#{l}: #{e.class}: #{e.message}"
end
e = {}
f = {a: 1}
a = [3, 1, 2]
h = {a: 1}
t("first e") { a.first(1, **e) }
t("first f") { a.first(1, **f) }
t("join f") { a.join("-", **f) }
t("key? e") { h.key?(:a, **e) }
t("key? f") { h.key?(:a, **f) }
t("between? e") { 2.between?(1, 3, **e) }
t("between? f") { 2.between?(1, 3, **f) }
t("each_slice f") { a.each_slice(2, **f) { } }
t("count block e") { a.count(**e) { |x| x > 1 } }
t("count block f") { a.count(**f) { |x| x > 1 } }
t("safe nav f") { z = a; z&.first(1, **f) }
t("send f") { a.send(:first, 1, **f) }
t("exist? e") { File.exist?("/", **e) }
t("exist? f") { File.exist?("/", **f) }
t("basename e") { File.basename("/a/b.rb", ".rb", **e) }
t("basename f") { File.basename("/a/b.rb", ".rb", **f) }
x = [a, "s"][0]
t("boxed size f") { x.size(**f) }
# a stream read back out of a container counts the full hash too
io = [$stdout, 1][0]
t("boxed flush f") { io.flush(**f) }
t("boxed closed? f") { io.closed?(**f) }
# without `require "io/console"` an IO has no winsize or noecho
t("boxed winsize f") { io.winsize(**f) }
t("boxed winsize pair") { io.winsize(a: 1) }
t("boxed noecho e") { io.noecho(**e) { |i| 5 } }
# a keyword fact holds only where it holds on each class one kind stands
# for: SizedQueue#push and #enq take timeout:
q = SizedQueue.new(3)
w = {timeout: 0.1}
t("sized push") { q.push(1, false, timeout: 0.1).size }
t("sized push e") { q.push(2, **e).size }
t("sized enq w") { q.enq(3, false, **w).size }
# Fiber#raise takes cause:, as Kernel#raise does
fb = Fiber.new { Fiber.yield 1; 2 }
fb.resume
t("fiber raise cause") { begin; fb.raise(RuntimeError, "x", nil, **{cause: nil}); rescue RuntimeError => x; x.message; end }
# a class held in a local reaches its class methods through the split
require "tmpdir"
pa = File.join(Dir.tmpdir, "spinel_kwsplat_kwless_local.txt")
File.write(pa, "abc")
k = IO
t("local for_fd e") { k.for_fd(k.sysopen(pa), "r", **e).gets }
kf = File
t("local basename e") { kf.basename("/a/b", **e) }
t("local basename f") { kf.basename("/a/b", **f) }
ki = Integer
t("local sqrt e") { ki.sqrt(16, **e) }
File.delete(pa)
