# spinel: gc-stress
# A C stack that runs out inside a green thread, a Fiber or an Enumerator body
# is the same SystemStackError the main thread's is, raised in that fiber: a
# rescue there takes it, and one that nothing in the body rescues ends the
# fiber the way any other raise does -- a Thread's surfaces at #join / #value,
# a Fiber's at #resume, an Enumerator's at #next. Spinel killed the process on
# the signal instead, after saying only that the fiber's stack had run out,
# and lost whatever the program had printed before it.
def deep(n) = n < 0 ? 0 : 1 + deep(n + 1)
def countdown(n) = n <= 0 ? 0 : 1 + countdown(n - 1)

puts "before"

# rescued inside the thread
r = Thread.new do
  begin
    deep(0)
  rescue SystemStackError => e
    "rescued: #{e.class}: #{e.message}"
  end
end.value
puts r

# unrescued: surfaces at join
# (the thread turns its own report off first: it runs in parallel with
# main, and can die before a `t.report_on_exception = false` after
# Thread.new runs, printing the report)
t = Thread.new { Thread.current.report_on_exception = false; deep(0) }
begin
  t.join
rescue SystemStackError => e
  puts "join: #{e.class}"
end
p t.status

# several in a row, each thread's stack usable after
3.times do |i|
  v = Thread.new do
    begin
      deep(0)
    rescue SystemStackError
      countdown(100)
    end
  end.value
  puts "thread #{i}: #{v}"
end

# inside a Fiber, rescued in the fiber
f = Fiber.new do
  begin
    deep(0)
  rescue SystemStackError
    Fiber.yield :fiber_rescued
  end
  :fiber_done
end
p f.resume
p f.resume

# unrescued in a Fiber: surfaces at resume
g = Fiber.new { deep(0) }
begin
  g.resume
rescue SystemStackError => e
  puts "resume: #{e.class}"
end
p g.alive?

# an external Enumerator whose body overflows
e = Enumerator.new do |y|
  y << 1
  y << deep(0)
end
p e.next
begin
  e.next
rescue SystemStackError
  puts "next: SystemStackError"
end

# ensure runs while the overflow unwinds the thread
log = []
Thread.new do
  begin
    deep(0)
  ensure
    log << :ensured
  end
rescue SystemStackError
  log << :rescued
end.join
p log

# the main thread is still fine
begin
  deep(0)
rescue SystemStackError
  puts "main: rescued"
end
acc = []
1000.times { |i| acc << "s#{i}" }
p acc.length
puts "after"
