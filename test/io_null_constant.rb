# IO::NULL is the null device, as File::NULL is: CRuby defines it on IO and
# File inherits it. The constant table knew it under File only, so a program
# naming it through IO warned at compile time and raised NameError.

p IO::NULL
p IO::NULL.frozen?
p IO::NULL == File::NULL
p File.exist?(IO::NULL)

# it flows like File's
def null_path = IO::NULL
p null_path
p [IO::NULL, File::SEPARATOR]

# and discards a stream
pid = Process.spawn("echo", "SHOULD-NOT-APPEAR", out: IO::NULL, err: IO::NULL)
Process.wait(pid)
puts "redirected"
