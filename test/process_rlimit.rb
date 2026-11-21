# spinel: int64
# spinel: gc-stress
# Process.getrlimit and Process.setrlimit take a resource as a Symbol, a
# String or a Process::RLIMIT_* Integer, and a limit as an Integer, a Float
# or the name INFINITY. getrlimit returns [soft, hard].
cur, max = Process.getrlimit(:NOFILE)
p Process.getrlimit("NOFILE") == [cur, max]
p Process.getrlimit(Process::RLIMIT_NOFILE) == [cur, max]
p cur.class
p max <= Process::RLIM_INFINITY

p Process.setrlimit(:NOFILE, 64, max)
p Process.getrlimit(:NOFILE)[0]
Process.setrlimit("NOFILE", 48.9, max)
p Process.getrlimit(:NOFILE)[0]
Process.setrlimit(Process::RLIMIT_CORE, 0)
p Process.getrlimit(:CORE)
Process.setrlimit(:CORE, 0, :INFINITY) rescue nil
p Process.getrlimit(:CORE)[0]

name = ARGV.empty? ? :CPU : "CPU"
soft, hard = Process.getrlimit(name)
p soft == hard || soft < hard

def show(e)
  p [e.class, e.message]
end

[:nofile, "FOO", "NOFILE\0x", :"NOFILE\0x", "no\nfile", "\u00e9\x01", "\x85".b,
 "\xED\xA0\x80", "\xED\xA0\x80\x01", "\u0080", "\u0085", "\u2028",
 :"no\x01file", :"\u00e9\x01"].each do |r|
  Process.getrlimit(r)
rescue ArgumentError => e
  show(e)
end
begin
  Process.getrlimit("\x01" * 1000)
rescue ArgumentError => e
  p e.message.bytesize
end
[nil, true, 2**40].each do |r|
  Process.getrlimit(r)
rescue TypeError, RangeError => e
  show(e)
end
[:FOO, :"FOO\0x", "SAVED", "INFINITY\0x", nil, true].each do |v|
  Process.setrlimit(:NOFILE, v)
rescue ArgumentError, TypeError => e
  show(e)
end
# The RangeError message differs by platform: rlim_t is unsigned long on
# Linux and unsigned long long on macOS
[2**70, -(2**70), 1.0e30, Float::NAN, -Float::INFINITY].each do |v|
  Process.setrlimit(:NOFILE, v)
rescue RangeError => e
  p e.class
end
begin
  Process.getrlimit(999)
rescue SystemCallError => e
  show(e)
end
begin
  Process.setrlimit(:NOFILE)
rescue ArgumentError => e
  show(e)
end
sigpending = begin
  Process::RLIMIT_SIGPENDING.class
rescue NameError
  nil
end
p sigpending.nil? || sigpending == Integer
# an object with to_str is read as its String, before to_int
class Name
  def initialize(s) = @s = s
  def to_str = @s
  def to_int = 7
end
class Num
  def to_int = 44
end
p Process.getrlimit(Name.new("NOFILE")) == Process.getrlimit(:NOFILE)
Process.setrlimit(Name.new("NOFILE"), Num.new, max)
p Process.getrlimit(:NOFILE)[0]
[Name.new("nofile"), Name.new("NO\0FILE")].each do |r|
  Process.getrlimit(r)
rescue ArgumentError => e
  show(e)
end
begin
  Process.setrlimit(:NOFILE, Name.new("NOFILE"))
rescue ArgumentError => e
  show(e)
end
# the soft limit converts in full, then the hard limit, then the resource
class Loud
  def initialize(s) = @s = s
  def to_str
    puts "to_str #{@s}"
    @s
  end
end
begin
  Process.setrlimit(Loud.new("FOO"), Loud.new("cur"), Loud.new("max"))
rescue ArgumentError => e
  show(e)
end
Process.setrlimit(:NOFILE, 40, nil)
p Process.getrlimit(:NOFILE)
p defined?(Process::RLIMIT_NOFILE)
p defined?(Process::RLIMIT_SIGPENDING) == (sigpending ? "constant" : nil)
