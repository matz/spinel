# system answers nil when the command cannot run, as CRuby does: no such
# program, with or without an environment Hash, or a one-String command
# line that needs no shell (its words are exec'd directly). A command line
# the shell runs keeps the shell's answer: false for its exit 127.
p system("nonexistent_cmd_q1", "a")
p system({"Z" => "1"}, "nonexistent_cmd_q1", "a")
p system({"Z" => "1"}, "nonexistent_cmd_q1")
p system("nonexistent_cmd_q1")
p $?.exitstatus
p system("nonexistent_cmd_q1 a b")
p system("nonexistent_cmd_q1 2>/dev/null")
p $?.exitstatus
p system("true")
p system("false")
p system("exit 3")
p $?.exitstatus
p system("echo  words   split")
p system("Z=1 true")
p system("test -n x")
p(system("true") ? "y" : "n")
p(system("nonexistent_q2") || "fallback")
p system(*["echo", "a b"])
p system({"Q" => "v"}, "sh", "-c", "echo $Q")
def run(c) = system(c)
p run("true"), run("nonexistent_q3")
p((Process.spawn("nonexistent_cmd_q1") rescue $!.class))
p((Process.spawn("nonexistent_cmd_q1 a b") rescue $!.class))
pid = Process.spawn("echo spawned  ok")
Process.wait(pid)
p $?.success?
p system("")
p $?.exitstatus

# spawn and exec of a missing program name it in the error
begin
  Process.spawn("no_such_program_xyz arg")
rescue SystemCallError => e
  p e.class, e.message
end
begin
  exec("no_such_program_xyz arg")
rescue SystemCallError => e
  p e.class, e.message
end
