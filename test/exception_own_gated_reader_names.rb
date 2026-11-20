# A program's exception class may define a method named like a reader a
# builtin exception class has (SystemExit#status, LoadError#path,
# UncaughtThrowError#tag, StopIteration#result, NameError#name, ...).
# Rescued under a class above it (`rescue => e`, `rescue Exception => e`),
# the call reaches the program's method, with or without a block, and a
# yielding one too; a builtin exception still answers its own reader, and
# any other raises NoMethodError.

class AppError < StandardError
  attr_reader :status, :path, :reason, :tag, :value, :signo, :signm, :exit_value, :result

  def initialize(msg, v)
    super(msg)
    @status = v
    @path = "p#{v}"
    @reason = :r
    @tag = :t
    @value = v * 2
    @signo = v + 1
    @signm = "SIG#{v}"
    @exit_value = [v]
    @result = v * 3
  end

  def success? = @status == 0
end

begin
  raise AppError.new("x", 9)
rescue => e
  p [e.status, e.path, e.reason, e.tag, e.value, e.signo, e.signm, e.exit_value, e.result, e.success?]
end

def probe(e)
  [
    (begin; e.status; rescue NoMethodError; :nm; end),
    (begin; e.success?; rescue NoMethodError; :nm; end),
    (begin; e.path; rescue NoMethodError; :nm; end),
    (begin; e.reason; rescue NoMethodError; :nm; end),
    (begin; e.tag; rescue NoMethodError; :nm; end),
    (begin; e.value; rescue NoMethodError; :nm; end),
    (begin; e.result; rescue NoMethodError; :nm; end),
  ]
end

[AppError.new("a", 0), SystemExit.new(4), LoadError.new("l"), RuntimeError.new("z")].each do |x|
  begin
    raise x
  rescue Exception => e
    p [e.class, probe(e)]
  end
end

begin; exit 3; rescue SystemExit => e; p [e.status, e.success?]; end
begin; raise LoadError, "m"; rescue LoadError => e; p e.path; end

# the answer is the method's, whatever it is
class PlainStatus < StandardError
  attr_reader :status
  def initialize(m, s) = (super(m); @status = s)
end
begin; raise PlainStatus.new("q", 1); rescue => e; p(e.status ? "y" : "n"); end
begin; raise PlainStatus.new("q", nil); rescue => e; p(e.status ? "y" : "n"); end
begin; raise PlainStatus.new("q", "str"); rescue => e; p e.status.upcase; end
begin; exit 5; rescue SystemExit, AppError => e; p [e.status, e.status.class]; end
def status_of(e) = e.status
begin; raise AppError.new("q", 4); rescue => e; p status_of(e); end
begin; exit 6; rescue Exception => e; p status_of(e); end
begin; raise "plain"; rescue => e; p((status_of(e) rescue $!.class)); end
begin; raise AppError.new("q", 4); rescue => e; p e&.status; end
x = begin; raise AppError.new("q", 7); rescue => e; e; end
p x.status

# a block goes to the program's method; a builtin reader leaves it unused
class BlockError < StandardError
  def initialize(m, r) = (super(m); @r = r)
  def receiver = block_given? ? yield(@r) : @r
  def key = @r
  def status(&b) = b ? b.call(@r) : -1
  def zap = block_given? ? yield(@r) : @r
end

begin
  raise BlockError.new("x", 3)
rescue => e
  p [e.receiver { |v| v * 2 }, e.receiver, e.status { |v| v + 10 }, e.status, e.zap, e.zap { |v| v + 1 }]
end
begin; {}.fetch(:zz); rescue => e; p [e.key { 1 }, e.receiver { 1 }]; end
begin; exit 6; rescue Exception => e; p e.status { 1 }; end
begin; nil.zork; rescue => e; p e.receiver { 2 }; p e.receiver; p((e.zap rescue :nm)); end

# NameError#name beside a program's own #name
class NamedError < StandardError
  def name = :own
end
begin; raise NamedError; rescue => e; p e.name; end
begin; zork; rescue NameError => e; p e.name; end
begin; nil.zork; rescue => e; p e.name; end
begin; raise "x"; rescue => e; p((e.name rescue :nm)); end
