# spinel: gc-stress
# A message a rescue just took in one thread is a root of that thread while
# another thread's collection runs. A worker that parks for that collection
# published its exception window without the messages and without the slot
# the rescue had popped, and left the slots above the window as they were,
# so the next push brought a stale one back in: under SPINEL_GC_STRESS=2 the
# collection freed the message and the thread's own next collection marked
# the freed String. The values a raise stages for its exception (a
# NoMethodError's receiver and arguments) went unpublished the same way.
def try
  yield
rescue => e
  e.message
end
ths = 3.times.map do |i|
  Thread.new do
    m = try { raise ArgumentError, "bad #{i}" }
    s = 0
    100.times { |k| s += "#{m}#{k}".size }
    [m, s]
  end
end
x = 0
100.times { |k| x += "main #{k}".size }
p ths.map(&:value), x
r = []
3.times do |i|
  r << Thread.new { try { raise ArgumentError, "bad #{i}" } }.value
  r << Thread.new { Fiber.new { try { Integer("x#{i}") } }.resume }.value
end
p r
# (each thread's own Array: `r` above is a top-level local, which a block's
# `r = []` would assign, so four threads running in parallel would push into
# one Array and the answer would depend on their timing)
nts = 4.times.map do |i|
  Thread.new do
    acc = []
    20.times do |k|
      begin
        k.zork("a#{i}", [k])
      rescue NoMethodError => e
        acc << e.class
      end
      acc << :x
    end
    acc.last(2)
  end
end
p nts.map(&:value)
