# SIGINFO, the status request of a BSD terminal (Ctrl-T): its name is in
# Signal.list where the platform has it (macOS, the BSDs), and trap and
# Signal.signame take it there; a shell lists it among the signals it can
# trap. Linux has no such signal, and the name is absent, as in CRuby.
has = Signal.list.key?("INFO")
p has == !!(RUBY_PLATFORM =~ /darwin|bsd/)
if has
  old = trap("INFO") { }
  p old, Signal.signame(Signal.list["INFO"]), Signal.list["INFO"] > 0
  trap("INFO", "DEFAULT")
else
  p "SYSTEM_DEFAULT", "INFO", true
end
