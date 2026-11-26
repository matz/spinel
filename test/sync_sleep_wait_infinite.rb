# Mutex#sleep and ConditionVariable#wait check the timeout before they let go
# of the mutex: Infinity and NaN are out of Time's range, a negative one is
# refused, and the mutex is still held after the raise; a Monitor keeps its
# depth.
require "monitor"

m = Mutex.new
cv = ConditionVariable.new

[Float::INFINITY, -Float::INFINITY, Float::NAN, -1.0].each do |t|
  r = m.synchronize do
    begin
      m.sleep(t)
      :slept
    rescue => e
      [e.class, e.message, m.owned?]
    end
  end
  p [:sleep, t, r]
  r = m.synchronize do
    begin
      cv.wait(m, t)
      :waited
    rescue => e
      [e.class, e.message, m.owned?]
    end
  end
  p [:wait, t, r]
end
p m.locked?

mon = Monitor.new
cond = mon.new_cond
[Float::INFINITY, -1.0].each do |t|
  r = mon.synchronize do
    mon.synchronize do
      begin
        cond.wait(t)
      rescue => e
        e.class
      end
    end
    mon.mon_owned?
  end
  p [:monitor, t, r, mon.mon_locked?]
end
