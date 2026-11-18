# An error the runtime raises itself (Array#fetch, Integer#/, Integer(), a
# past-the-end Enumerator#next, File.open) is built in CRuby through the
# #initialize of its class, so a program's reopening of a builtin exception
# that defines one runs on it: the message it makes is what a rescue reads.
# NameError, NoMethodError and KeyError raised by the runtime are not built
# that way (CRuby fills their name, receiver and key itself), and a reopened
# initialize does not see them.
$calls = 0
$log = []

class StandardError
  def initialize(msg = nil)
    $calls += 1
    super("S[#{msg}]")
  end
end

class ZeroDivisionError
  def initialize(msg = nil) = super("Z<" + msg.to_s + ">")
end

class IndexError
  def initialize(m = "C:" + self.class.name)
    $log << "idx"
    super(m)
  end
end

class StopIteration
  def initialize(m = nil)
    super("SI(" + m.to_s + ")")
  end
end

def t(label)
  yield
rescue => e
  puts "#{label}: #{e.class} #{e.message.inspect}"
end

t("fetch") { [1].fetch(9) }
t("zero") { 1 / 0 }
t("type") { 1 + "a" }
t("integer") { Integer("zz") }
t("float") { Float("zz") }
t("range") { [1, 2].first(-1) }
t("stop") { [].each.next }
t("errno") { File.open("/nonexistent/x") }
t("nomethod") { nil.foo }
t("key") { {a: 1}.fetch(:b) }
p $log

# the message and the attributes the runtime gave it
begin
  {a: 1}.fetch(:b)
rescue KeyError => e
  p e.key, e.receiver
end

# one run of the initialize per raise: a re-raise, an ensure and a rescue
# modifier do not run it again
before = $calls
begin
  begin
    [1].fetch(9)
  rescue => e
    raise
  end
rescue => e2
  p e2.message, $calls - before, e2.equal?(e)
end
before = $calls
begin
  begin
    [1].fetch(9)
  ensure
    $ensured = true
  end
rescue => e3
  p e3.message, $calls - before
end
before = $calls
x = [1].fetch(9) rescue $!.message
p x, $calls - before
