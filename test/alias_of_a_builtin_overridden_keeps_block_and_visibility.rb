# The forwarder an alias of a builtin becomes, where the class defines the
# name after it, hands the caller's block on and keeps the builtin's
# visibility under a `private` section.
class Store < Hash
  alias_method :raw_each, :each
  private
  alias_method :raw_store, :[]=
  public

  def each(&b)
    super(&b)
  end

  def []=(k, v)
    raw_store(k.to_s, v)
  end
end

s = Store.new
s[:a] = 1
s.raw_store("b", 2)
out = []
s.raw_each { |k, v| out << "#{k}=#{v}" }
p out
p s
