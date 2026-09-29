class Img
  def initialize(v) = @v = v
  def v = @v
  def method_missing(name, *args, **opts)
    "op #{name} on #{@v} with #{args.inspect}"
  end
  def self.method_missing(name, *args, **opts)
    Img.new("#{name}(#{args.join(",")})")
  end
  def respond_to_missing?(name, all = false) = true
end
im = Img.black(10, 5)
p im.v
p im.avg
p im.linear(1, 2)
