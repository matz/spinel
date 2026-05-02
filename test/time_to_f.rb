t = Time.now
puts t.to_i > 0
puts t.to_f > 0.0
ms = (Time.now.to_f * 1000).to_i
puts ms > 0

# Sub-second precision: pre-fix Time.now returned whole-second mrb_int,
# so t.to_f always equalled t.to_i.to_f. With clock_gettime resolution,
# at least one of 50 samples will have a nonzero fractional component.
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
