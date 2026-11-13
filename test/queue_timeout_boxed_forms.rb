# spinel: gc-stress
# Queue's timeout: forms on a boxed receiver, and what a boxed value that
# is no Queue answers to them. Only a Queue takes the keyword: an Array
# reads the Hash as its count, pushes it, or takes one argument to <<, a
# Hash's shift takes no argument, anything else has no such method.
def t(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

def box(v) = [v, 1, "s"][0]

t("pop(true, timeout:)") { box(Thread::Queue.new).pop(true, timeout: 1) }
t("pop(true, timeout: nil)") { box(Thread::Queue.new).pop(true, timeout: nil) }
t("pop(false, timeout:)") { q = Thread::Queue.new; q << 4; box(q).pop(false, timeout: 0.01) }
t("shift(nil, timeout:)") { q = Thread::Queue.new; q << 5; box(q).shift(nil, timeout: 1) }
t("deq(true, timeout: nil)") { q = Thread::Queue.new; q << 6; box(q).deq(true, timeout: nil) }
t("pop(timeout: 0)") { box(Thread::Queue.new).pop(timeout: 0) }
t("pop(timeout: -1)") { box(Thread::Queue.new).pop(timeout: -1) }
t("pop(timeout: Rational)") { box(Thread::Queue.new).pop(timeout: 1r / 100) }
t("closed, an item left") { q = Thread::Queue.new; q << 9; q.close; box(q).pop(timeout: 0.01) }
t("closed, empty") { q = Thread::Queue.new; q.close; box(q).pop(timeout: 0.01) }

# SizedQueue#push / enq / <<(obj, non_block, timeout:)
full = Thread::SizedQueue.new(1)
full << 1
t("push full") { box(full).push(2, timeout: 0.01) }
t("push(false) full") { box(full).push(2, false, timeout: 0.01) }
t("enq full") { box(full).enq(2, timeout: 0.01) }
t("<< full") { box(full).<<(2, timeout: 0.01) }
t("push(true, timeout:)") { box(full).push(2, true, timeout: 1) }
t("push timeout: nil, room") { box(Thread::SizedQueue.new(2)).push(3, timeout: nil).size }
t("push room") { box(Thread::SizedQueue.new(2)).push(3, timeout: 0.01).size }
t("push closed") { s = Thread::SizedQueue.new(1); s.close; box(s).push(1, timeout: 0.01) }
t("Queue push(x, timeout:)") { box(Thread::Queue.new).push(1, timeout: 1) }
t("Queue enq(x, timeout:)") { box(Thread::Queue.new).enq(1, timeout: 1) }
t("Queue push(x, false, timeout:)") { box(Thread::Queue.new).push(1, false, timeout: 1) }
t("Queue <<(x, timeout:)") { box(Thread::Queue.new).<<(1, timeout: 1) }
pusher = Thread.new(full) { |w| w.push(2, timeout: 5).size }
Thread.pass until pusher.status == "sleep" || !pusher.alive?
t("pop makes room") { full.pop }
t("waiting push") { pusher.value }

# a boxed value that is no Queue
t("Array pop") { box([1]).pop(timeout: 1) }
t("Array shift") { box([1]).shift(timeout: 1) }
t("Array pop(true, timeout:)") { box([1]).pop(true, timeout: 1) }
t("Array deq") { box([1]).deq(timeout: 1) }
t("Array push") { box([1, "a"]).push(2, timeout: 1) }
t("Array push(x, false, timeout:)") { box([1, "a"]).push(2, false, timeout: 1) }
t("Array enq") { box([1]).enq(2, timeout: 1) }
t("Array <<") { box([1]).<<(2, timeout: 1) }
t("String <<") { box(+"s").<<("x", timeout: 1) }
t("nil <<") { box(nil).<<(1, timeout: 1) }
t("Hash shift") { box({ a: 1 }).shift(timeout: 1) }
t("Hash shift(true, timeout:)") { box({ a: 1 }).shift(true, timeout: 1) }
t("Hash pop") { box({ a: 1 }).pop(timeout: 1) }
t("String pop") { box("x").pop(timeout: 1) }
t("nil pop") { box(nil).pop(timeout: 1) }
t("Integer pop") { box(1).pop(timeout: 1) }

# the timeout converts as CRuby's does, typed or boxed: a String, true or
# false is no Float
tq = Thread::Queue.new
t("typed String timeout") { tq.pop(timeout: "1") }
t("typed false timeout") { tq.pop(timeout: false) }
t("typed push true timeout") { full.push(2, timeout: true) }
t("boxed String timeout") { box(tq).pop(timeout: "1") }
t("boxed true timeout") { box(tq).shift(timeout: true) }
t("boxed push String timeout") { box(full).push(2, timeout: +"1") }

# the receiver and the arguments are read once each, left to right, and a
# pushed value outlives an allocating timeout expression
log = []
q = Thread::Queue.new
q << :x
p [log << :recv, q][1].pop((log << :non_block; false), timeout: (log << :timeout; 0.5))
p log
def padded_timeout(n)
  a = []
  n.times { |i| a << ("pad" * 20) + i.to_s }
  a.size * 0.0 + 0.5
end
sq = box(Thread::SizedQueue.new(4))
bad = 0
100.times do |i|
  sq.push("val-" + i.to_s, timeout: padded_timeout(20))
  bad += 1 unless sq.pop == "val-" + i.to_s
end
p bad
