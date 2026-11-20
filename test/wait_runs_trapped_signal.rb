# A trapped signal that arrives while the program waits for a child runs its
# handler then, as in CRuby, and the wait goes on for the child. The wait of
# a program with one thread retried waitpid(2) on EINTR without running the
# pending handler, and the handler was lost: it ran neither during the wait
# nor after it. (With a second thread alive the wait polls and parks, and
# that path ran the handler.)
trap("TERM") { puts "handler ran" }

pid = spawn("sh", "-c", "sleep 0.3; kill -TERM $PPID; sleep 0.7")
Process.wait(pid)
puts "wait returned #{$?.exitstatus}"

system("sh", "-c", "sleep 0.3; kill -TERM $PPID; sleep 0.7")
puts "system returned #{$?.exitstatus}"

pid = spawn("sh", "-c", "sleep 0.3; kill -TERM $PPID; sleep 0.7")
pid2, st = Process.wait2(pid)
puts "wait2 returned #{pid2 == pid} #{st.exitstatus}"

# and the default SIGINT, which this program leaves as it is, raises Interrupt
# in the wait, not after the child
pid = spawn("sh", "-c", "sleep 0.3; kill -INT $PPID; sleep 0.7")
begin
  Process.wait(pid)
  puts "wait returned"
rescue Interrupt
  puts "Interrupt in the wait"
end
Process.wait(pid) rescue nil

# a trap for CHLD, which the child's own end records: its proc runs once the
# wait has answered, before the program goes on, even when the wait was the
# program's last line
trap("CHLD") { puts "CHLD ran" }
pid = spawn("sleep", "0.3")
Process.wait(pid)
puts "wait returned #{$?.exitstatus}"

# the backtick waits too: a CHLD trap that raises lands in the rescue
# around it
trap("CHLD") { raise "CHLD raised" }
begin
  out = `echo from the child`
  puts "backtick returned #{out.inspect}"
rescue => e
  puts e.message
end
