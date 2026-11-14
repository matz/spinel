# An undef on main's singleton of a method the core gives main, with no
# method of the program's of the name, is a version too: where the record
# cannot tell whether it has run, the call raises when the undef's cell
# says it ran (NameError for a variable-like call, NoMethodError for
# every other form) and is the builtin's call otherwise, main's own to_s
# answering "main".

p to_s
if ARGV.size == 0
  class << self; undef_method :to_s; end
end
p((to_s rescue $!.class))
p((to_s() rescue $!.class))
p((self.to_s rescue $!.class))
p(([1].map { to_s } rescue $!.class))
# (inspect is main's own method still)
p inspect

# not run: the builtin answers
if ARGV.size == 9
  class << self; undef_method :inspect; end
end
p((inspect rescue $!.class))
p((self.inspect rescue $!.class))

# a Kernel method main answers
if ARGV.size == 0
  class << self; undef_method :frozen?; end
end
p((frozen? rescue $!.class))
p((self.frozen? rescue $!.class))
p Object.new.frozen?

# a private Kernel method, not undefined: still called as main's own
if ARGV.size == 9
  class << self; undef_method :puts; end
end
puts "kept"
[1].each { puts "kept in a block" }

# not run: splats, keywords, a literal block, a frame reader in main's
# own method and a receiver another call reads keep the builtin's call
if ARGV.size == 9
  class << self
    undef_method :p
    undef_method :block_given?
    undef_method :catch
    undef_method :binding
  end
end
a = [4, 5]
w = p(*a)
p w, p(*[])
p(k: 1)
def self.m = block_given?
p m { }, m
p catch(:t) { throw :t, 6 }
x = 7
p binding.local_variable_get(:x)

# run: an operator form, and a call that is a rescue modifier's value
if ARGV.size == 0
  class << self
    undef_method :==
    undef_method :print
  end
end
p((self == self rescue $!.class))
p((self.==(self) rescue $!.class))
$stdout.puts((print rescue $!.class).inspect)
$stdout.puts((print("x") rescue $!.class).inspect)

# run: the call's operands are evaluated ahead of the lookup that raises,
# in order: arguments, a splat's to_a, a `**`'s to_hash, a `&`'s to_proc;
# as a statement, a value, a receiver, an interpolation's part and another
# such call's argument
def side(x) = ($log << x; x)
class Conv
  def to_a = ($log << :to_a; [1])
  def to_hash = ($log << :to_hash; {k: 1})
  def to_proc = ($log << :to_proc; proc { })
end
$log = []
o = Conv.new
if ARGV.size == 0
  class << self
    undef_method :format
    undef_method :Integer
    undef_method :sprintf
  end
end
begin; format(side("%d"), side(1)); rescue NoMethodError => e; $stdout.puts e.class; end
begin; format(side("%d"), *o); rescue NoMethodError; end
begin; sprintf(side("%d"), **o); rescue NoMethodError; end
begin; sprintf(side("%d"), k: side(2), &o); rescue NoMethodError; end
$stdout.puts $log.inspect; $log.clear
v = [side(:a), (format(side("%s"), side(3)) rescue :err), side(:b)]
$stdout.puts v.inspect, $log.inspect; $log.clear
v = (Integer(side("5")).succ rescue :err)
s = ("<#{format(side("%s"), side(4))}>" rescue :err)
$stdout.puts v.inspect, s.inspect, $log.inspect; $log.clear
v = (format(side("%s"), Integer(side("6"))) rescue :err)
$stdout.puts v.inspect, $log.inspect; $log.clear
y = 1
v = (format(side("%d %d"), y, (y = side(7))) rescue :err)
$stdout.puts v.inspect, $log.inspect, y
