# A `...` method_missing whose `super` no program method answers (Object's,
# which raises NoMethodError) forwards its arguments to the call it makes:
# the `super` hands them nowhere

class Collector
  def html(*args, &block) = block ? "html #{args.inspect} #{block.call}" : "html #{args.inspect}"
  def json(*args) = "json #{args.size}"

  def method_missing(symbol, ...)
    if (format = symbol.to_s.delete_suffix("_now")) != symbol.to_s
      public_send(format.to_sym, ...)
    else
      super
    end
  end
end

c = Collector.new
p c.method_missing(:html_now)
p c.method_missing(:html_now, 1, 2)
p c.method_missing(:json_now, :a, :b, :c)
p(c.method_missing(:html_now, 3) { "blk" })
begin
  c.method_missing(:xml, 1)
rescue NoMethodError
  p :no_method
end
