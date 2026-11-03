# A SIGINT that arrives while the program recurses through a program `new`
# whose super reaches the builtin Class#new: that super enters the
# initialize of the class it builds, so the recursion is a cycle of the
# call graph and polls (#9222).
$stdout.sync = true
pid = Process.pid
Process.spawn("sh", "-c", "sleep 0.2; kill -INT #{pid}")
class Fa
  attr_reader :v
  def initialize(n)
    @v = n < 2 ? n : Fb.new(n - 1).v + Fb.new(n - 2).v
  end
end
class Fb < Fa
  def self.new(n) = super(n)
end
begin
  p Fb.new(60).v
rescue Interrupt
  puts "recursion interrupted"
end
