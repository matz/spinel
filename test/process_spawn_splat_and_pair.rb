# spinel: gc-stress
# Process.spawn spreads a splat into the command and its arguments at run
# time, as the literal list is, and runs [program, argv0] with argv0 as the
# program's argv[0] (#7192).
def spawn_splat(*args) = Process.waitpid2(Process.spawn(*args))

Process.waitpid2(Process.spawn("printf", "%s|", "a", "b"))
puts
spawn_splat("printf", "%s|", "a", "b")
puts
args = ["printf", "%s|", "a", "b"]
Process.waitpid2(Process.spawn(*args))
puts
Process.waitpid2(Process.spawn(*["echo hi"]))
Process.waitpid2(Process.spawn(*["pwd"], chdir: "/"))
Process.waitpid2(Process.spawn(["printf", "argv0"], "%s|", "a"))
puts
rest = ["%s-", "x"]
Process.waitpid2(Process.spawn("printf", *rest))
puts
begin
  Process.spawn(["printf"])
rescue ArgumentError => e
  p e
end
begin
  Process.spawn(*[])
rescue ArgumentError => e
  p e
end
