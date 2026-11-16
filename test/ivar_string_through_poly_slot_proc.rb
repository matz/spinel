# A String instance variable handed to a proc read out of a slot that holds
# other values too (Logger's `@formatter.call(..., @progname, ...)`): a proc
# that appends to it appends to the ivar's String, as in CRuby
class L
  def initialize(f)
    @formatter = f
    @progname = +"prog"
  end
  attr_reader :progname
  def write(m) = @formatter.call(@progname, m)
end
f = ARGV[0] ? "x" : ->(pn, m) { pn << "!"; m }
l = L.new(f)
p l.write("hi")
p l.progname
