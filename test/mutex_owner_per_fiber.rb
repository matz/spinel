# A Mutex or Monitor held by one Fiber is not owned by another Fiber of the
# same thread: owned?, try_lock, try_enter and mon_owned? are false, lock
# and unlock raise ThreadError; the holding Fiber re-enters a Monitor
require "monitor"

def other_fiber_view(m, mon)
  out = []
  out << m.owned?
  out << m.try_lock
  begin
    m.unlock
  rescue ThreadError => e
    out << e.message
  end
  begin
    m.lock
  rescue ThreadError => e
    out << e.message
  end
  out << mon.try_enter
  out << mon.mon_owned?
  out
end

def run_pair
  m = Mutex.new
  mon = Monitor.new
  holder = Fiber.new do
    m.lock
    mon.enter
    Fiber.yield [m.owned?, mon.mon_owned?, mon.try_enter]
    mon.exit
    mon.exit
    m.unlock
    :released
  end
  first = holder.resume
  seen = Fiber.new { other_fiber_view(m, mon) }.resume
  last = holder.resume
  [first, seen, last, m.locked?, mon.mon_locked?]
end

run_pair.each { |row| p row }
Thread.new { run_pair.each { |row| p row } }.join(10)

m = Mutex.new
m.synchronize { p [m.owned?, m.try_lock] }
mon = Monitor.new
mon.synchronize { mon.synchronize { p [mon.mon_owned?, mon.try_enter] }; mon.exit }
p mon.mon_locked?

# a Fiber that ended holding the lock still holds it: no later Fiber owns it
ended = Mutex.new
Fiber.new { ended.lock }.resume
GC.start
results = 200.times.map do
  Fiber.new do
    [ended.owned?, (ended.unlock; :unlocked)]
  rescue ThreadError => e
    [ended.owned?, e.message]
  end.resume
end
p results.uniq, ended.locked?
