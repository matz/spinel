# $? is the Process::Status of the last child waited for -- by
# Kernel#system, a backtick, Process.wait or Process.wait2 -- and nil
# before any. It was typed Integer (the raw status word), so
# `$?.exitstatus` raised NoMethodError; Process.wait and Process.wait2
# were not available at all.
p $?
p $?.nil?
p $?.class
s = $?
p s.nil?
# nil has none of the readers; it answers to_s, inspect, to_i and == as nil
# (the class, name and receiver: Ruby 3.2 and 3.4 word the message apart)
begin; $?.exitstatus; rescue NoMethodError => e; p [e.class, e.name, e.receiver]; end
begin; $?.success?; rescue NoMethodError => e; p [e.class, e.name, e.receiver]; end
begin; s.pid; rescue NoMethodError => e; p [e.class, e.name, e.receiver]; end
p [$?.to_s, $?.inspect, $?.to_i, $? == 0, $? != 0]
system("sh", "-c", "exit 2")
p $?.nil?
p $?.inspect.sub(/pid \d+/, "pid N")
p $?.to_s.sub(/pid \d+/, "pid N")
puts "#{$?.exitstatus}"
p [$?].map(&:exitstatus)
system("true")
st = $?
p st.class
p [st.success?, st.exitstatus, st.exited?, st.to_i]
p $?.pid == st.pid
p st.pid > 0

system("sh", "-c", "exit 3")
p [$?.success?, $?.exitstatus, $?.to_i >> 8]
p $? == 768
p $?.pid > 0

out = `echo hi`
p out
p [$?.success?, $?.exitstatus]

pid = Process.spawn("sh", "-c", "exit 4")
Process.wait(pid)
p [$?.success?, $?.exitstatus, $?.pid == pid]

pid = Process.spawn("sh", "-c", "exit 0")
wpid, st2 = Process.wait2(pid)
p [wpid == pid, st2.success?, st2.exitstatus]
p [$?.success?, $?.exitstatus, $?.pid == pid]

def last_ok
  $?.success?
end
system("false")
p last_ok
statuses = []
[0, 5].each do |code|
  system("sh", "-c", "exit #{code}")
  statuses << $?
end
p statuses.map(&:exitstatus)
p statuses.map(&:class)
p "status #{$?.exitstatus}"

# a slot that held an Integer before it held $? compares by the status word,
# as the in-tree tools' `$sh_status == 0` does
$last = 0
system("true")
$last = $?
p [$last == 0, 0 == $last, $last == 1]
system("sh", "-c", "exit 1")
$last = $?
p [$last == 0, $last == 256, $last.exitstatus]
