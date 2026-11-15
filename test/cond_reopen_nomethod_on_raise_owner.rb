# A def in a reopening of Fiber or Thread under a run-time condition that
# did not run: the call raises NoMethodError, as CRuby's does. The
# dispatcher's arm for no method raised through a bare `raise`, which in a
# method of a class that answers raise itself is that class's: Fiber#raise
# raised FiberError on the unborn fiber, and Thread's arm did not compile
# (#11819).
$skip = true    # the guarded code does not run
$take = false   # the guarded code runs

unless $skip
  class Fiber
    def tag = :fiber_tag
  end
end
unless $take
  class Fiber
    def mark = :fiber_mark
  end
end

unless $skip
  class Thread
    def tag = :thread_tag
    def count = 1   # an Integer answer: the arm's C did not compile
  end
end
unless $take
  class Thread
    def mark = :thread_mark
  end
end

def try
  yield
rescue NoMethodError => e
  [e.class, e.message, e.name, e.receiver.class]
end

f = Fiber.new { }
p try { f.tag }
p try { f.mark }
p try { Thread.current.tag }
p try { Thread.current.mark }
p try { Thread.current.count }
