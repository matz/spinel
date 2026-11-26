# Monitor#enter, #exit, #try_enter and their mon_ names lock and unlock a
# Monitor as #synchronize does, reentrantly
require "monitor"

m = Monitor.new
p m.try_enter
p m.mon_locked?
p m.try_enter
m.exit
m.exit
p m.mon_locked?
p m.enter
p m.mon_owned?
p m.exit
m.mon_enter
p m.mon_try_enter
p m.try_mon_enter
m.mon_exit
m.mon_exit
m.mon_exit
p m.mon_locked?

m.enter
p Thread.new { m.try_enter }.join(5).value
m.exit
p m.mon_locked?

begin
  Mutex.new.try_enter
rescue NoMethodError => e
  puts e.message
end
