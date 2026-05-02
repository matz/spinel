# Smoke: Time.now compiles and yields a positive timestamp.
puts Time.now.to_i > 0

# Without .to_f: Time.now.to_i is whole seconds. In a tight loop of
# 100 samples virtually every read lands in the same second (the loop
# runs in << 1s). Cap at 1 so the output is deterministic.
secs = []
i = 0
while i < 100
  secs.push(Time.now.to_i)
  i = i + 1
end
sn = secs.uniq.length
if sn > 1
  sn = 1
end
puts sn

# With .to_f: Time.now.to_f is float seconds with sub-second precision.
# Scale the fractional part to microseconds; virtually every sample
# lands in a distinct bucket. Pre-fix Time.now was whole-second mrb_int,
# so every sample's microsecond bucket was 0 → 1 distinct value. With
# clock_gettime nanosecond resolution we see many. Cap at 2 so the
# output is deterministic: 1 = no sub-second precision, 2 = present.
micros = []
i = 0
while i < 100
  s = Time.now
  micros.push(((s.to_f - s.to_i) * 1_000_000).to_i)
  i = i + 1
end
mn = micros.uniq.length
if mn > 2
  mn = 2
end
puts mn

puts "done"
