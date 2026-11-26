# A block whose last expression is a call on a nil-or-object receiver,
# answered by nil's method or the class's own, gives that answer as the
# value of the yield that ran it
class Entry
  def initialize(dn) = @dn = dn
  def dn = @dn
  def ==(other) = other.instance_of?(self.class) && @dn == other.dn
  def label = "entry #{@dn}"
  def self.parse(str)
    return nil if str.empty?
    Entry.new(str)
  end
end

class NilClass
  def label = "none"
end

def show
  v = yield
  p v
end

e = Entry.new("a")
show { Entry.parse("a") == e }
show { Entry.parse("") == e }
show { Entry.parse("b") == e }
show { nil }
show { Entry.parse("a").label }
show { Entry.parse("").label }

def run_only
  yield
  p :ran
end
run_only { Entry.parse("a") == e }

# a block with redo takes the same value form
tries = 0
show do
  tries += 1
  redo if tries < 3
  Entry.parse("a") == e
end
show do
  tries += 1
  redo if tries < 5
  Entry.parse("").label
end
p tries
