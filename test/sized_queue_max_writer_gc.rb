# spinel: gc-stress
# SizedQueue#max= with an argument whose evaluation allocates, on a queue
# that only the call holds (a method's result), keeps the queue alive
# while the argument is evaluated and boxed.
def make_queue = SizedQueue.new(1)

n = 0
300.times { |i| n += 1 if (make_queue.max = Array.new(2, i).sum + 2.5) }
p n
