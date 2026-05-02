t = Time.now

# Basic shape: positive integer seconds and positive float seconds.
puts t.to_i > 0
puts t.to_f > 0.0

# to_f >= to_i (to_i truncates the fractional part).
puts t.to_f >= t.to_i

# Millisecond idiom: (Time.now.to_f * 1000).to_i is the wall-clock ms
# count; must be positive and at least seconds*1000.
ms = (t.to_f * 1000).to_i
puts ms > 0
puts ms >= t.to_i * 1000

# Two consecutive reads don't go backward.
a = Time.now
b = Time.now
puts b >= a

# Sub-second precision: pre-fix Time.now returned whole-second mrb_int,
# so t.to_f always equalled t.to_i.to_f. With clock_gettime resolution,
# at least one of 50 samples has a nonzero fractional part — odds of
# all 50 hitting tv_nsec == 0 are effectively zero.
found = false
i = 0
while i < 50
  s = Time.now
  if s.to_f != s.to_i.to_f
    found = true
  end
  i = i + 1
end
puts found

puts "done"
