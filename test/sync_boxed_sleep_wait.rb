# Mutex#sleep and ConditionVariable#wait on a Mutex and a ConditionVariable
# read back out of a Hash or an Array, beside a user class with its own
# sleep and wait, a receiverless sleep, a value other than a Mutex passed
# to wait, and sleep on a receiver other than a Mutex.
class Napper
  def sleep(t = nil) = [:napper_sleep, t]
  def wait(m, t = nil) = [:napper_wait, t]
end

m = Mutex.new
cvs = {1 => ConditionVariable.new}
locks = {1 => m}
m.synchronize { p cvs[1].wait(m, 0.01) }
m.synchronize { p locks[1].sleep(0.01) }

sleepers = [m, Napper.new]
waiters = [ConditionVariable.new, Napper.new]
m.synchronize { p sleepers[0].sleep(0.01) }
p sleepers[1].sleep(0.01)
m.synchronize { p waiters[0].wait(m, 0.01) }
p waiters[1].wait(m, 0.01)

p sleep(0.01) <= 1   # whole seconds slept: 0, or 1 across a second boundary
p cvs[1].wait(1, 0.01) <= 1
[-1, -0.5, Float::NAN, Float::INFINITY, -Float::INFINITY].each do |t|
  m.synchronize { cvs[1].wait(m, t) }
rescue ArgumentError, RangeError => e
  puts "#{e.class}: #{e.message}"
end
begin
  cvs[1].wait(1, Float::NAN)
rescue RangeError => e
  puts "RangeError: #{e.message}"
end
begin
  m.synchronize { locks[1].sleep(Float::INFINITY) }
rescue RangeError => e
  puts "RangeError: #{e.message}"
end
m.synchronize { p cvs[1].wait(m, 0) }

others = [5, m]
begin
  others[0].sleep(0.01)
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end

waker = nil
m.synchronize do
  waker = Thread.new { m.synchronize { cvs[1].signal } }
  p cvs[1].wait(m, 5).nil?
end
waker.join(5)
p %i[lock unlock try_lock locked? owned? synchronize sleep].map { |x| locks[1].respond_to?(x) }
p %i[wait signal broadcast].map { |x| cvs[1].respond_to?(x) }
require "monitor"
mon = Monitor.new
bmon = [mon, 1][0]
begin
  mon.synchronize { bmon.sleep(0.01) }
rescue NoMethodError => e
  puts "NoMethodError: #{e.message}"
end
p bmon.respond_to?(:sleep), bmon.respond_to?(:lock)
p(mon.synchronize { cvs[1].wait(mon, 0.01) } <= 1)
p :done

# a receiver that runs code runs before an argument whose boxed values_at
# runs code
$order = []
sync = [Mutex.new, ConditionVariable.new, 1]
sm = sync[0]
waits = [[0.001], "x"]
wv = waits[0]
def sync_pick(sync, i)
  $order << :recv
  sync[i]
end
sm.synchronize { sync_pick(sync, 0).sleep(wv.values_at(($order << :arg; 0))[0]) }
sm.synchronize { (($order << :recv); sync[1]).wait(sm, wv.values_at(($order << :arg; 0))[0]) }
sm.synchronize { sync[($order << :recv; 1)].wait(sm, wv.values_at(($order << :arg; 0))[0]) }
p $order
