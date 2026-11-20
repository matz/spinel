# spinel: gc-minor
# A block call holds its receiver while the block runs. The Array element
# produced by String unary plus still identifies the mutable source.
class UnaryPlusEntry
  def initialize(source)
    @source = source
  end

  def fetch
    [+@source].map { |entry| GC.start; entry }.first
  end
end

def check(source)
  entry = UnaryPlusEntry.new(source).fetch
  p [entry.equal?(source), entry.frozen?]
  entry << "!"
  p [source, entry]
end

check(String.new("a\0b"))
check("a\0b")
