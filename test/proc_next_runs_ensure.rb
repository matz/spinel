# A `next` in a proc or lambda runs the `ensure` it leaves and answers its
# value, and pops the handler of a begin/rescue it leaves. The proc's C
# function returned from inside the region instead: the ensure did not run,
# the value was nil, and the handler frame stayed on the stack.
c = ARGV.length == 0

# the begin is the proc's value
pr1 = proc { begin; next 5 if c; 7; ensure; puts "e1"; end }
p pr1.call

# the begin is a statement, the value comes from a parameter
pr2 = proc do |x|
  begin
    next x * 2 if c
    puts "not reached"
  ensure
    puts "e2 #{x}"
  end
  7
end
p pr2.call(4), pr2.call(5)

# a String, a Float, an Array, several values, none
ps = proc { |s| begin; next s + "!" if c; s; ensure; puts "e3"; end }
puts ps.call("a")
pf = proc { |f| begin; next f * 2.0 if c; f; ensure; puts "e4"; end }
p pf.call(1.5)
pa = proc { |a| begin; next [a, a] if c; []; ensure; puts "e5"; end }
p pa.call(3)
pm = proc { begin; next 1, "b" if c; 7; ensure; puts "e6"; end }
p pm.call
pn = proc do
  begin
    next if c
    puts "not reached"
  ensure
    puts "e7"
  end
  7
end
p pn.call

# the tail has no value of its own
pt = proc { |x| begin; next x if c; ensure; puts "e8"; end; puts "not reached" }
p pt.call(4)

# two regions: the inner ensure, then the outer
pr3 = proc do |x|
  begin
    begin
      next "v#{x}" if c
      puts "not reached"
    ensure
      puts "inner"
    end
  ensure
    puts "outer"
  end
  "tail"
end
p pr3.call(1)

# from the rescue clause and from the else clause
pr4 = proc do
  begin
    raise "x"
  rescue => e
    next e.message if c
    puts "not reached"
  ensure
    puts "e9"
  end
  7
end
p pr4.call
pr5 = proc do |x|
  begin
    x += 1
  rescue
    puts "not reached"
  else
    next "else #{x}" if c
    puts "not reached"
  ensure
    puts "e10"
  end
  "tail"
end
p pr5.call(1)

# a `next` of an iterator's block inside the region is that block's
pr6 = proc do
  out = []
  begin
    [1, 2, 3].each do |i|
      next if i == 2
      out << i
    end
    next out if c
    puts "not reached"
  ensure
    puts "e11"
  end
  7
end
p pr6.call

# a lambda, both spellings
l7 = lambda do |x|
  begin
    next x + 1 if c
    0
  ensure
    puts "e12"
  end
end
p l7.call(1)
l8 = ->(x) { begin; next :a if c; x; ensure; puts "e13"; end }
p l8.(1)

# Mutex#synchronize is such a region: the lock is released
m = Mutex.new
stop = lambda do
  m.synchronize do
    next if c
    puts "not reached"
  end
end
stop.call
p m.locked?
stop.call
p m.locked?

# a begin/rescue with no ensure: each call leaves its handler frame, so the
# stack of them does not fill up
pr9 = proc do |x|
  begin
    next x + 1 if c
    puts "not reached"
  rescue
    puts "not reached"
  end
  7
end
s = 0
300.times { |i| s += pr9.call(i) }
p s
pr10 = proc do
  begin
    raise "x"
  rescue
    next if c
    puts "not reached"
  end
  7
end
300.times { pr10.call }
p pr10.call
begin
  raise "after"
rescue => e
  puts e.message
end
