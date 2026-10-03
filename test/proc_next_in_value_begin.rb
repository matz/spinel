# A `next` or a lambda's `return` inside a begin whose value is used leaves
# the proc with its value. The begin collects its value in a temp, and the
# exit stored its value where the begin's goes: the proc answered nil, and a
# lambda's `return` there did not build.
c = ARGV.length == 0

# the begin is the proc's last statement
p1 = proc { begin; next 5 if c; 7; rescue; 0; end }
p p1.call
p2 = proc { |x| begin; next x if c; "s"; rescue; 0; end }
p p2.call(4)
p3 = proc { |x| begin; next x + "!" if c; "s"; rescue; "r"; end }
puts p3.call("a")
p4 = proc { begin; next if c; 7; rescue; 0; end }
p p4.call

# the begin is assigned
p5 = proc do |x|
  y = begin
    next x if c
    "s"
  rescue
    1
  end
  [y]
end
p p5.call(4)

# an ensure inside such a begin hands the value to the proc, not to the begin
p6 = proc do |x|
  begin
    begin
      next x if c
    ensure
      puts "e6"
    end
    "s"
  rescue
    1
  end
end
p p6.call(4)
p7 = proc do |x|
  y = begin
    begin
      next x if c
    ensure
      puts "e7"
    end
    "s"
  rescue
    1
  end
  [y]
end
p p7.call(4)

# a lambda's return
l1 = lambda { begin; return 5 if c; 7; rescue; 0; end }
p l1.call
l2 = lambda { |x| begin; return x + 1 if c; x; rescue; 0; end }
p l2.call(1) + 1
l3 = lambda { |s| begin; return s + "!" if c; s; rescue; "r"; end }
puts l3.call("a")
l4 = lambda { |x| y = begin; return x + 1 if c; x; ensure; puts "e11"; end; y * 2 }
p l4.call(1)
l5 = lambda do |x|
  begin
    begin
      return x if c
    ensure
      puts "e12"
    end
    "s"
  rescue
    1
  end
end
p l5.call(4)

# not taken, the begin's own value is the proc's
p8 = proc { |x| begin; next 5 if x > 9; x * 2; rescue; 0; end }
p p8.call(4)
p9 = proc { |x| begin; next 5 if x > 9; raise "r"; rescue => e; e.message; end }
p p9.call(4)

# a return through a block's own region inside such a begin
m = Mutex.new
l6 = lambda { |x| begin; m.synchronize { return x if x > 0 }; "s"; rescue; 1; end }
p l6.call(4)
p m.locked?
l7 = lambda { |a| begin; a.select! { |v| return v if v > 2; true }; "s"; rescue; 1; end }
p l7.call([1, 2, 3])
