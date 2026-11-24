# A recursion that keeps a begin/rescue, an ensure or a catch armed at every
# level fills the runtime's handler stack long before the C stack. That is
# CRuby's SystemStackError too, and a program that rescues it goes on
# running: the frame on top of the stack is armed and complete, and the
# raise lands in it, as it does when the C stack runs out
# (test/stack_overflow_rescued.rb). Spinel ended the process there instead,
# with the message on stderr and nothing of the program's rescue or ensure.
def deep_rescue(n)
  begin
    deep_rescue(n + 1) + 1
  rescue ZeroDivisionError
    -1
  end
end
begin
  deep_rescue(0)
rescue SystemStackError => e
  puts "rescue frames: #{e.class}"
end

# every ensure of the frames that are unwound runs, one for each frame
# entered; CRuby, whose innermost ensure body runs on the stack that just ran
# out, can lose that one, so the innermost is allowed
$ensures = 0
$entered = 0
def deep_ensure(n)
  begin
    $entered += 1
    deep_ensure(n + 1)
  ensure
    $ensures += 1
  end
end
begin
  deep_ensure(0)
rescue SystemStackError
  puts "ensure frames: rescued, ensures missed beyond the innermost: #{$entered - $ensures > 1}"
end

# an arm that takes something before its frame (Mutex#synchronize its lock)
# raises before it does, so nothing is left taken by the frame that never
# armed
$ms = []
def deep_sync(n)
  m = Mutex.new
  $ms << m
  m.synchronize { deep_sync(n + 1) }
end
begin
  deep_sync(0)
rescue SystemStackError
  puts "synchronize frames: rescued, all unlocked: #{$ms.none?(&:locked?)}"
end

# a catch held open at every level
def deep_catch(n)
  catch(:t) { deep_catch(n + 1) + 1 }
end
begin
  deep_catch(0)
rescue SystemStackError
  puts "catch frames: rescued"
end

# the stacks are usable again afterwards, twice over
2.times do |i|
  begin
    deep_rescue(0)
  rescue SystemStackError
    puts "again #{i}"
  end
end

# a bounded recursion that holds a frame at every level runs to its end, as
# CRuby's does
def bounded(n)
  begin
    return 0 if n <= 0
    bounded(n - 1) + 1
  rescue ZeroDivisionError
    -1
  end
end
p bounded(500)
def bounded_catch(n)
  return 0 if n <= 0
  catch(:t) { bounded_catch(n - 1) + 1 }
end
p bounded_catch(500)

# the same inside a thread and a fiber, whose handler stacks are their own
t = Thread.new do
  begin
    deep_rescue(0)
  rescue SystemStackError
    "thread: rescued"
  end
end
puts t.value
f = Fiber.new do
  begin
    deep_ensure(0)
  rescue SystemStackError
    "fiber: rescued"
  end
end
puts f.resume

# the program goes on, and the collector with it
acc = []
1000.times { |i| acc << "s#{i}" }
p acc.length
puts "end"
