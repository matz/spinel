# A SIGINT that arrives while the program recurses through `new` with no
# loop: a builtin `new` enters the initialize of the class it builds, so
# Fib#initialize is on a cycle of the call graph and polls at its entry
# (#9222), as a recursion through its own name does.
$stdout.sync = true
pid = Process.pid
Process.spawn("sh", "-c", "sleep 0.2; kill -INT #{pid}")
class Fib
  attr_reader :v
  def initialize(n)
    @v = n < 2 ? n : Fib.new(n - 1).v + Fib.new(n - 2).v
  end
end
begin
  p Fib.new(60).v
rescue Interrupt
  puts "recursion interrupted"
end
