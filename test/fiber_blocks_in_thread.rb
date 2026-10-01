# A green thread that resumes a Fiber which then blocks (sleep, Queue#pop,
# Thread.pass) stops inside that Fiber. The scheduler resumed it at the
# thread's own fiber, whose context #resume had saved elsewhere, and crashed;
# a #kill or #raise of it was never delivered in the Fiber.

def wait_sleeping(t)
  Thread.pass while t.status and t.status != "sleep"
end

# sleep inside a Fiber
t = Thread.new { r = Fiber.new { sleep 0.01; 1 }.resume; [r, 2] }
p t.value

# Queue#pop inside a Fiber
q = Queue.new
t = Thread.new { Fiber.new { q.pop * 10 }.resume }
wait_sleeping(t)
q << 7
p t.value

# Thread.pass inside a Fiber inside a Fiber, which also yields
t = Thread.new do
  log = []
  outer = Fiber.new do
    inner = Fiber.new { 3.times { |i| Thread.pass; log << i }; :inner }
    log << inner.resume
    Fiber.yield :outer
    log << :back
  end
  log << outer.resume
  Thread.pass
  outer.resume
  log
end
p t.value

# #wakeup ends a sleep inside a Fiber
t = Thread.new { Fiber.new { sleep; :woke }.resume }
wait_sleeping(t)
t.wakeup
p t.value

# #kill unwinds the Fiber (its ensure runs) and then the thread
log = []
t = Thread.new do
  begin
    Fiber.new do
      begin
        sleep
      ensure
        log << :fiber_ensure
      end
    end.resume
    log << :not_reached
  ensure
    log << :thread_ensure
  end
end
wait_sleeping(t)
t.kill
t.join
p log, t.status

# #raise is delivered in the Fiber, where a rescue sees it
t = Thread.new do
  Fiber.new do
    begin
      sleep
    rescue => e
      "fiber rescued #{e.message}"
    end
  end.resume
end
wait_sleeping(t)
t.raise("boom")
p t.value

# and an unrescued one goes on up through #resume to the thread
t = Thread.new do
  begin
    Fiber.new { sleep }.resume
  rescue => e
    "thread rescued #{e.message}"
  end
end
wait_sleeping(t)
t.raise("bang")
p t.value
p :done
