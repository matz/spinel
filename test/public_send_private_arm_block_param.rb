# public_send handing on a block parameter (`&blk`, an anonymous `&`) to a
# name one of whose candidates is private: the private arm refuses the call
# without the block, whether or not a block was passed

class Collector
  def html(*args, &block) = block ? "html #{args.inspect} #{block.call}" : "html #{args.inspect}"

  def relay(symbol, *, &)
    public_send(symbol, *, &)
  end

  def relay_named(symbol, *rest, &blk)
    public_send(symbol, *rest, &blk)
  end

  private

  def secret(*) = :secret
end

c = Collector.new
p c.relay(:html, 1, 2)
p(c.relay(:html, 3) { "anon" })
p c.relay_named(:html)
p(c.relay_named(:html, 4) { "named" })
[:relay, :relay_named].each do |m|
  begin
    c.public_send(m, :secret, 5)
  rescue NoMethodError => e
    p e.message
  end
  begin
    c.public_send(m, :secret) { :never }
  rescue NoMethodError => e
    p e.message
  end
end
