# Queue#pop(timeout:) on a queue a block takes boxed: Thread.new's
# argument, a Fiber's resume value, an element of a mixed Array. The
# keyword was read as Array#pop's count and raised TypeError.
q = Thread::Queue.new
p Thread.new(q) { |w| w.pop(timeout: 0.01) }.value
p Thread.new(q) { |w| w.shift(timeout: 0.01) }.value
p Thread.new(q) { |w| w.deq(timeout: 0.01) }.value
p Thread.new(q, 0.01) { |w, t| w.pop(timeout: t) }.value
q << 7
p Thread.new(q) { |w| w.pop(timeout: 0.01) }.value
q << 8
p Thread.new(q) { |w| w.pop(timeout: nil) }.value

# a method that one call site gives the thread's argument
def wait_on(queue, timeout)
  queue.pop(timeout: timeout)
end
Thread.new(q) { |w| p wait_on(w, 0.01) }.join
p wait_on(q, 0.01)

# a worker started with its wakeup queue, which stop resets to nil
module Flusher
  def self.start
    @wakeup = Thread::Queue.new
    @thread = Thread.new(@wakeup) { |wakeup| run(wakeup) }
  end

  def self.run(wakeup)
    wakeups = 0
    wakeups += 1 while wakeup.pop(timeout: 5) == :flush
    wakeups
  end

  def self.stop
    @wakeup << :flush << :flush << :stop
    n = @thread.value
    @thread = @wakeup = nil
    n
  end
end
Flusher.start
p Flusher.stop

# a Fiber's resume value and an element of a mixed Array
p Fiber.new { |w| w.pop(timeout: 0.01) }.resume(q)
mixed = [q, 1, "s"]
p mixed[0].pop(timeout: 0.01)

# a waiting pop answers what another thread pushes
waiter = Thread.new(q) { |w| w.pop(timeout: 5) }
Thread.pass until waiter.status == "sleep" || !waiter.alive?
q << :woken
p waiter.value
