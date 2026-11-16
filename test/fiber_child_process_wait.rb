# A child process waited for from inside a Fiber, with no other thread
# (#12024): system, backticks and an IO.popen whose child is still running
# when the read starts. The scheduler had not started, and the waits took
# its absence for another thread and switched out to none (a SIGSEGV in
# sp_ctx_swap). The same calls on the main fiber, and inside a Fiber
# while a Thread runs, are answered as before.
f = Fiber.new { puts "system: #{system("true")}"; 1 }
puts "resume: #{f.resume}"
g = Fiber.new { x = `echo bt`; p x; 2 }
p g.resume
h = Fiber.new { IO.popen("sleep 0.2; echo hi") { _1.read } }
p h.resume
p system("true")
t = Thread.new { 3 }
k = Fiber.new { [system("true"), `echo th`] }
p k.resume
p t.value
