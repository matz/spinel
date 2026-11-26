# Mutex#lock and #synchronize (and Monitor#synchronize) raise ThreadError
# in a trap proc, as CRuby's do; try_lock, unlock, locked? and owned? work
# there, and the main thread locks again once the proc has returned or
# raised out.
require "monitor"

m = Mutex.new
mon = Monitor.new
got = []
Signal.trap("USR1") do
  begin
    m.synchronize { got << :synchronized }
  rescue ThreadError => e
    got << "synchronize: #{e.message}"
  end
  begin
    m.lock
    got << :locked
  rescue ThreadError => e
    got << "lock: #{e.message}"
  end
  begin
    mon.synchronize { got << :monitor }
  rescue ThreadError => e
    got << "monitor: #{e.message}"
  end
  got << [m.try_lock, m.locked?, m.owned?]
  m.unlock
  got << m.locked?
  m.try_lock
  begin
    ConditionVariable.new.wait(m, 0)
  rescue ThreadError => e
    got << "wait: #{e.message}"
  end
  got << m.owned?
end
Process.kill("USR1", Process.pid)
sleep 0.05
got.each { |g| p g }
m.synchronize { p :main_locks }

Signal.trap("USR2") { raise "from trap" }
begin
  Process.kill("USR2", Process.pid)
  sleep 0.05
rescue RuntimeError => e
  p e.message
end
m.lock
p m.owned?
m.unlock
