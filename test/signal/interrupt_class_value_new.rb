# A SIGINT that arrives while the program recurses through `new` on a
# class value of two classes: the plan cannot name the class it builds, so
# the call may enter any initialize of the program, and Fa#initialize is on
# a cycle of the call graph and polls at its entry (#9222).
$stdout.sync = true
pid = Process.pid
Process.spawn("sh", "-c", "sleep 0.2; kill -INT #{pid}")
class Fa
  attr_reader :v
  def initialize(n, k)
    @v = n < 2 ? n : k.new(n - 1, k).v + k.new(n - 2, k).v
  end
end
class Fb < Fa; end
k = ARGV.size > 5 ? Fa : Fb
begin
  p k.new(60, k).v
rescue Interrupt
  puts "recursion interrupted"
end
