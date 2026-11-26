# Signal.trap with a Method runs the method, nil and "" ignore the signal,
# and "EXIT" exits through at_exit; each handler reads back as CRuby's
# (the Method, nil, "IGNORE", "EXIT").
def on_hup(signo)
  puts "on_hup #{signo}"
end

Signal.trap("HUP", method(:on_hup))
Process.kill("HUP", Process.pid)
sleep 0.05
p Signal.trap("HUP", "DEFAULT").class
hup_proc = method(:on_hup).to_proc
Signal.trap("HUP", hup_proc)
p Signal.trap("HUP", "DEFAULT").equal?(hup_proc)

Signal.trap("USR2", nil)
Process.kill("USR2", Process.pid)
sleep 0.05
puts "alive after nil"
p Signal.trap("USR2", "")
Process.kill("USR2", Process.pid)
sleep 0.05
puts "alive after empty"
p Signal.trap("USR2", "SIG_IGN")
p Signal.trap("USR2", "SYSTEM_DEFAULT")
p Signal.trap("USR2", "DEFAULT")

at_exit { puts "at_exit ran" }
Signal.trap("TERM", "EXIT")
p Signal.trap("TERM", "EXIT")
Process.kill("TERM", Process.pid)
sleep 1
puts "not reached"
