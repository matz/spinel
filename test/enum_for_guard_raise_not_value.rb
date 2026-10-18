# An each-like method whose block form answers nothing anyone keeps, whose
# `begin` has a rescue clause that is a `raise`. A call
# that never returns is no value: the method keeps the Enumerator its
# `return to_enum unless block_given?` guard answers as its return, as
# it does without the `raise`.
class Readable
  def initialize(chunks) = @chunks = chunks
  def read = @chunks.shift
  def each
    return to_enum unless block_given?
    begin
      while chunk = read
        yield chunk
      end
    rescue => error
      raise
    end
  end
end
e = Readable.new(["a", "b", "c"]).each
p e.map(&:upcase)
p Readable.new(["x", "y"]).each.with_index.to_a
p Readable.new(["m"]).each { |c| c }
