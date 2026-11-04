# A SIGINT that arrives while the program recurses through `new` on a
# class value that may be a class with a program `new` (Fb) or one that
# takes the builtin `new` (Fa): the call enters Fb.new or Fa#initialize,
# so Fa#initialize is on a cycle of the call graph and polls (#9222).
$stdout.sync = true
pid = Process.pid
Process.spawn("sh", "-c", "sleep 0.2; kill -INT #{pid}")
class Fa
  attr_reader :v
  def initialize(n, k)
    @v = n < 2 ? n : k.new(n - 1, k).v + k.new(n - 2, k).v
  end
end
class Fb < Fa
  def self.new(n, k) = super(n, k)
end
k = ARGV.size > 5 ? Fb : Fa
begin
  p k.new(60, k).v
rescue Interrupt
  puts "recursion interrupted"
end
