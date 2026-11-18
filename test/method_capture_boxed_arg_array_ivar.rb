# The boxed argument of method_capture_boxed_arg.rb, with the Method kept in
# an instance variable and in an Array beside a lambda.
class Registry
  def initialize(handler)
    @handler = handler
    @list = [handler, ->(v) { v }]
  end
  def run(v) = @handler.call(v)
  def first(v) = @list[0].call(v)
  def each(v) = @list.map { |h| h.call(v) }
end
def describe(value)
  value.class.name
end
p describe(1)
r = Registry.new(method(:describe))
p r.run(2.5)
p r.first("s")
p r.each(:sym)
p r.run(nil)
p r.run([1, 2])
p describe(3)
