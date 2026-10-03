# A Fiber, Thread or Enumerator block with an `ensure` builds in a method
# that a proc returns from. The block is a C function of its own, and its
# ensure ended in a jump to the method's return label, which that function
# does not have.
def mm
  pr = proc { return 1 }
  f = Fiber.new { begin; 3; ensure; puts "e"; end }
  f.resume
end
p mm

def with_fiber(n)
  pr = proc { return -1 }
  f = Fiber.new { begin; n + 1; ensure; puts "fiber ensure"; end }
  v = f.resume
  pr.call if n > 5
  v
end
p with_fiber(1)
p with_fiber(9)

def with_thread(n)
  pr = proc { return "early" }
  t = Thread.new do
    begin
      "t#{n}"
    ensure
      puts "thread ensure"
    end
  end
  v = t.value
  pr.call if n > 5
  v
end
puts with_thread(1)
puts with_thread(9)

def with_enum(n)
  pr = proc { return [] }
  e = Enumerator.new do |y|
    begin
      y << n
      y << n + 1
    ensure
      puts "enum ensure"
    end
  end
  v = e.to_a
  pr.call if n > 5
  v
end
p with_enum(1)
p with_enum(9)

def with_rescue(n)
  pr = proc { return :early }
  f = Fiber.new do
    begin
      raise "boom" if n > 0
      :quiet
    rescue => e
      e.message
    ensure
      puts "ensure #{n}"
    end
  end
  v = f.resume
  pr.call if n > 5
  v
end
p with_rescue(0)
p with_rescue(1)
p with_rescue(9)

def with_next(n)
  pr = proc { return :early }
  f = Fiber.new do
    begin
      next :left if n > 0
      :stayed
    ensure
      puts "ensure #{n}"
    end
  end
  v = f.resume
  pr.call if n > 5
  v
end
p with_next(0)
p with_next(1)
p with_next(9)
