# In a threaded program a collection waits at a safepoint poll for every
# thread. A nested walk leaves its poll to the enclosing loop (#9940) and a
# builtin iterator's step (map, ...) takes none, as a pass is short; one that
# reads what a String holds (a length that counts characters) is not, and
# polls at each pass. One that reads only the bytesize takes no poll.
# (infer-test checks the polls; here the walks run beside a thread that
# allocates, so collections land in them.)

def nested_walk(xs, ws, reps)
  t = 0
  reps.times { xs.each { |x| t += ws[x].length } }
  t
end

def slow_map(ws) = ws.map { |w| w.length }
def tight_map(ws) = ws.map { |w| w.bytesize }

ws = Array.new(300) { |i| "é" * (20 + i % 5) }
xs = Array.new(2000) { |i| i % 300 }

$stop = false
$started = false
churn = Thread.new do
  junk = Array.new(64) { |k| "s#{k}" }.size
  $started = true   # (so a walk that ends first cannot stop it before it allocates)
  until $stop
    junk += Array.new(64) { |k| "s#{k}" }.size
    Thread.pass
  end
  junk > 0
end

work = Thread.new do
  [nested_walk(xs, ws, 20), slow_map(ws).sum, tight_map(ws).sum]
end
p work.value
Thread.pass until $started
$stop = true
p churn.value
