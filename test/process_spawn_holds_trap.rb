# spinel: not-cruby -- CRuby runs a trap proc inside Process.spawn
# A trap proc does not run between the fork in Process.spawn and the return
# of the pid. A child sends USR1 2000 times, and the parent spawns until the
# child exits.
spawning = false
seen = 0
trap("USR1") { seen += 1 if spawning }
killer = Process.spawn("sh", "-c", "for i in $(seq 2000); do kill -USR1 $PPID 2>/dev/null || exit; done")
puts "spawning"
loop do
  spawning = true
  pid = Process.spawn("true")
  spawning = false
  Process.wait(pid)
  break if Process.wait(killer, Process::WNOHANG)
end
p seen
